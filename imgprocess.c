/*
 * imgprocess.c — Parallel image processing engine
 * Applies grayscale + edge detection to a batch of PPM images.
 *
 * Usage:
 *   ./imgprocess --procs <N> --threads <T> [--verbose] <input_dir> <output_dir>
 *
 * IPC used:
 *   - fork()        : create N worker processes
 *   - pipe()        : workers report progress to master
 *   - shmget/shmat  : shared task queue (ring buffer of filenames)
 *   - semaphore     : protect shared memory access (POSIX sem)
 *   - pthreads      : T threads per worker process
 *
 * Build:
 *   gcc -O2 -o imgprocess imgprocess.c -lpthread -lm
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <errno.h>

/* ── tunables ──────────────────────────────────────────────────── */
#define MAX_FILES     4096
#define MAX_PATH      512
#define SHM_QUEUE_CAP 4096   /* shared task queue capacity          */
#define SHM_KEY       0x1A2B /* arbitrary IPC key                   */

/* ── PPM image (P6 binary) ─────────────────────────────────────── */
typedef struct {
    int w, h;
    unsigned char *data; /* RGB interleaved, w*h*3 bytes */
} Image;

/* ── shared task queue (in shared memory) ─────────────────────── */
typedef struct {
    sem_t  mutex;
    int    head, tail, count, cap;
    char   paths[SHM_QUEUE_CAP][MAX_PATH];
} SharedQueue;

/* ── globals ───────────────────────────────────────────────────── */
static int        g_verbose    = 0;
static char       g_outdir[MAX_PATH];
static SharedQueue *g_queue    = NULL;

/* ── helpers ───────────────────────────────────────────────────── */
static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ── PPM I/O ───────────────────────────────────────────────────── */
static Image *ppm_read(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char magic[3];
    if (!fgets(magic, 3, f) || strncmp(magic, "P6", 2) != 0) { fclose(f); return NULL; }
    Image *img = calloc(1, sizeof(Image));
    int maxval;
    if (fscanf(f, " %d %d %d ", &img->w, &img->h, &maxval) != 3) { free(img); fclose(f); return NULL; }
    size_t sz = (size_t)img->w * img->h * 3;
    img->data = malloc(sz);
    if (fread(img->data, 1, sz, f) != sz) { free(img->data); free(img); fclose(f); return NULL; }
    fclose(f);
    return img;
}

static void ppm_free(Image *img) { if (img) { free(img->data); free(img); } }

static int ppm_write(const char *path, Image *img) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", img->w, img->h);
    fwrite(img->data, 1, (size_t)img->w * img->h * 3, f);
    fclose(f);
    return 0;
}

/* ── image filters ─────────────────────────────────────────────── */
static void filter_grayscale(Image *img) {
    unsigned char *p = img->data;
    for (int i = 0; i < img->w * img->h; i++, p += 3) {
        unsigned char g = (unsigned char)(0.299*p[0] + 0.587*p[1] + 0.114*p[2]);
        p[0] = p[1] = p[2] = g;
    }
}

static void filter_edge(Image *img) {
    /* Sobel operator on grayscale luminance */
    int w = img->w, h = img->h;
    unsigned char *src = img->data;
    unsigned char *dst = malloc((size_t)w * h * 3);
    static const int Gx[3][3] = {{-1,0,1},{-2,0,2},{-1,0,1}};
    static const int Gy[3][3] = {{-1,-2,-1},{0,0,0},{1,2,1}};
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int gx = 0, gy = 0;
            for (int ky = -1; ky <= 1; ky++) {
                for (int kx = -1; kx <= 1; kx++) {
                    int ny = y + ky, nx = x + kx;
                    if (ny < 0) ny = 0;
                    if (ny >= h) ny = h-1;
                    if (nx < 0) nx = 0;
                    if (nx >= w) nx = w-1;
                    int lum = src[(ny*w+nx)*3]; /* already grayscale */
                    gx += Gx[ky+1][kx+1] * lum;
                    gy += Gy[ky+1][kx+1] * lum;
                }
            }
            int mag = (int)sqrt((double)gx*gx + (double)gy*gy);
            if (mag > 255) mag = 255;
            dst[(y*w+x)*3] = dst[(y*w+x)*3+1] = dst[(y*w+x)*3+2] = (unsigned char)mag;
        }
    }
    memcpy(img->data, dst, (size_t)w*h*3);
    free(dst);
}

/* ── process one image file ────────────────────────────────────── */
static void process_file(const char *inpath, int pipe_fd) {
    Image *img = ppm_read(inpath);
    if (!img) {
        if (g_verbose) fprintf(stderr, "[worker %d] failed to read: %s\n", getpid(), inpath);
        return;
    }
    filter_grayscale(img);
    filter_edge(img);

    /* build output path */
    const char *base = strrchr(inpath, '/');
    base = base ? base+1 : inpath;
    char outpath[MAX_PATH];
    snprintf(outpath, sizeof(outpath), "%s/%s", g_outdir, base);
    ppm_write(outpath, img);
    ppm_free(img);

    /* report progress via pipe */
    char msg = 1;
    write(pipe_fd, &msg, 1);
    if (g_verbose) printf("[pid %d] done: %s\n", getpid(), base);
}

/* ── thread worker ─────────────────────────────────────────────── */
typedef struct { int pipe_fd; } ThreadArg;

static void *thread_worker(void *arg) {
    ThreadArg *ta = (ThreadArg *)arg;
    while (1) {
        /* pop one task from shared queue */
        sem_wait(&g_queue->mutex);
        if (g_queue->count == 0) { sem_post(&g_queue->mutex); break; }
        char path[MAX_PATH];
        strncpy(path, g_queue->paths[g_queue->head], MAX_PATH-1);
        g_queue->head = (g_queue->head + 1) % g_queue->cap;
        g_queue->count--;
        sem_post(&g_queue->mutex);
        process_file(path, ta->pipe_fd);
    }
    return NULL;
}

/* ── worker process ────────────────────────────────────────────── */
static void worker_process(int num_threads, int pipe_fd) {
    ThreadArg ta = { .pipe_fd = pipe_fd };
    pthread_t *threads = malloc((size_t)num_threads * sizeof(pthread_t));
    for (int i = 0; i < num_threads; i++)
        pthread_create(&threads[i], NULL, thread_worker, &ta);
    for (int i = 0; i < num_threads; i++)
        pthread_join(threads[i], NULL);
    free(threads);
    close(pipe_fd);
    exit(0);
}

/* ── collect image files ───────────────────────────────────────── */
static int collect_files(const char *dir, char files[][MAX_PATH], int cap) {
    DIR *d = opendir(dir);
    if (!d) { perror("opendir"); return -1; }
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) && n < cap) {
        size_t len = strlen(ent->d_name);
        if (len < 4) continue;
        const char *ext = ent->d_name + len - 4;
        if (strcmp(ext, ".ppm") != 0) continue;
        snprintf(files[n], MAX_PATH, "%s/%s", dir, ent->d_name);
        n++;
    }
    closedir(d);
    return n;
}

/* ── benchmark helper ──────────────────────────────────────────── */
typedef struct {
    int procs, threads;
    double elapsed;
    double speedup;
} BenchResult;

/* ── run one benchmark configuration ──────────────────────────── */
static double run_config(const char *indir, const char *outdir,
                         int num_procs, int num_threads,
                         char files[][MAX_PATH], int nfiles) {
    /* set up shared memory queue */
    int shmid = shmget(SHM_KEY, sizeof(SharedQueue), IPC_CREAT | 0666);
    if (shmid < 0) { perror("shmget"); return -1; }
    g_queue = (SharedQueue *)shmat(shmid, NULL, 0);
    if (g_queue == (void*)-1) { perror("shmat"); return -1; }

    sem_init(&g_queue->mutex, 1 /* pshared */, 1);
    g_queue->head = g_queue->tail = g_queue->count = 0;
    g_queue->cap = SHM_QUEUE_CAP;
    for (int i = 0; i < nfiles && i < SHM_QUEUE_CAP; i++) {
        strncpy(g_queue->paths[i], files[i], MAX_PATH-1);
        g_queue->tail = i + 1;
        g_queue->count++;
    }

    strncpy(g_outdir, outdir, MAX_PATH-1);
    mkdir(outdir, 0755);

    /* create pipes: one per worker process */
    int (*pipes)[2] = malloc((size_t)num_procs * 2 * sizeof(int));

    double t0 = now_sec();

    pid_t *pids = malloc((size_t)num_procs * sizeof(pid_t));
    for (int i = 0; i < num_procs; i++) {
        pipe(pipes[i]);
        pids[i] = fork();
        if (pids[i] == 0) {
            close(pipes[i][0]);
            worker_process(num_threads, pipes[i][1]);
        }
        close(pipes[i][1]);
    }

    /* master: read progress from all pipes */
    int done = 0;
    fd_set rfds;
    while (done < nfiles) {
        FD_ZERO(&rfds);
        int maxfd = 0;
        for (int i = 0; i < num_procs; i++) {
            if (pipes[i][0] >= 0) { FD_SET(pipes[i][0], &rfds); if (pipes[i][0] > maxfd) maxfd = pipes[i][0]; }
        }
        if (select(maxfd+1, &rfds, NULL, NULL, NULL) < 0) break;
        for (int i = 0; i < num_procs; i++) {
            if (pipes[i][0] >= 0 && FD_ISSET(pipes[i][0], &rfds)) {
                char buf[64]; int n = read(pipes[i][0], buf, sizeof(buf));
                if (n <= 0) { close(pipes[i][0]); pipes[i][0] = -1; }
                else done += n;
            }
        }
    }
    for (int i = 0; i < num_procs; i++) waitpid(pids[i], NULL, 0);
    double elapsed = now_sec() - t0;

    sem_destroy(&g_queue->mutex);
    shmdt(g_queue);
    shmctl(shmid, IPC_RMID, NULL);
    g_queue = NULL;

    free(pids);
    free(pipes);
    return elapsed;
}

/* ── main ──────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    int num_procs = 1, num_threads = 1;
    int benchmark_mode = 0;
    char indir[MAX_PATH] = "", outdir[MAX_PATH] = "";

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--procs")   && i+1<argc) { num_procs   = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--threads") && i+1<argc) { num_threads = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--verbose"))              { g_verbose   = 1; }
        else if (!strcmp(argv[i], "--benchmark"))            { benchmark_mode = 1; }
        else if (indir[0]  == '\0') strncpy(indir,  argv[i], MAX_PATH-1);
        else if (outdir[0] == '\0') strncpy(outdir, argv[i], MAX_PATH-1);
    }

    if (indir[0] == '\0') {
        fprintf(stderr, "Usage: %s [--procs N] [--threads T] [--verbose] [--benchmark] <indir> [outdir]\n", argv[0]);
        return 1;
    }
    if (outdir[0] == '\0') strncpy(outdir, "output", MAX_PATH-1);

    static char files[MAX_FILES][MAX_PATH];
    int nfiles = collect_files(indir, files, MAX_FILES);
    if (nfiles <= 0) { fprintf(stderr, "No .ppm files found in %s\n", indir); return 1; }
    printf("Found %d image(s) in %s\n", nfiles, indir);

    if (benchmark_mode) {
        /* ── full benchmark sweep ── */
        int proc_configs[]   = {1, 2, 4, 4, 8};
        int thread_configs[] = {1, 2, 2, 4, 2};
        int ncfg = 5;
        BenchResult results[5];
        double baseline = -1;

        printf("\n%-12s %-10s %-12s %-10s\n", "Procs", "Threads", "Time (s)", "Speedup");
        printf("%-12s %-10s %-12s %-10s\n", "-----", "-------", "--------", "-------");

        for (int c = 0; c < ncfg; c++) {
            char cfgdir[MAX_PATH];
            snprintf(cfgdir, sizeof(cfgdir), "%s_p%dt%d", outdir, proc_configs[c], thread_configs[c]);
            double t = run_config(indir, cfgdir, proc_configs[c], thread_configs[c], files, nfiles);
            if (c == 0) baseline = t;
            results[c].procs   = proc_configs[c];
            results[c].threads = thread_configs[c];
            results[c].elapsed = t;
            results[c].speedup = baseline / t;
            printf("%-12d %-10d %-12.2f %-10.2f\n",
                   results[c].procs, results[c].threads,
                   results[c].elapsed, results[c].speedup);
        }
        printf("\nBest speedup: %.2fx \n",
               results[0].speedup < results[4].speedup ? results[4].speedup : results[1].speedup);
    } else {
        /* ── single run ── */
        double t = run_config(indir, outdir, num_procs, num_threads, files, nfiles);
        printf("Done. %d image(s) processed in %.2fs  [%d proc(s) x %d thread(s)]\n",
               nfiles, t, num_procs, num_threads);
    }
    return 0;
}
