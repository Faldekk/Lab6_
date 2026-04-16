//#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SHOP_FILENAME "./shop"
#define MIN_SHELVES 8
#define MAX_SHELVES 256
#define MIN_WORKERS 1
#define MAX_WORKERS 64

#define ERR(source)                                     \
    do                                                  \
    {                                                   \
        fprintf(stderr, "%s:%d\n", __FILE__, __LINE__); \
        perror(source);                                 \
        kill(0, SIGKILL);                               \
        exit(EXIT_FAILURE);                             \
    } while (0)

void usage(char* program_name)
{
    fprintf(stderr, "Usage: \n");
    fprintf(stderr, "\t%s n m\n", program_name);
    fprintf(stderr, "\t  n - number of items (shelves), %d <= n <= %d\n", MIN_SHELVES, MAX_SHELVES);
    fprintf(stderr, "\t  m - number of workers, %d <= m <= %d\n", MIN_WORKERS, MAX_WORKERS);
    exit(EXIT_FAILURE);
}

void ms_sleep(unsigned int milli)
{
    time_t sec = (int)(milli / 1000);
    milli = milli - (sec * 1000);
    struct timespec ts = {0};
    ts.tv_sec = sec;
    ts.tv_nsec = milli * 1000000L;
    if (nanosleep(&ts, &ts))
        ERR("nanosleep");
}

void swap(int* x, int* y)
{
    int tmp = *y;
    *y = *x;
    *x = tmp;
}

void shuffle(int* array, int n)
{
    for (int i = n - 1; i > 0; i--)
    {
        int j = rand() % (i + 1);
        swap(&array[i], &array[j]);
    }
}

void print_array(int* array, int n)
{
    for (int i = 0; i < n; ++i)
    {
        printf("%3d ", array[i]);
    }
    printf("\n");
}

void safe_lock_mutex(pthread_mutex_t* mutex, int shelf_idx, int* workers_alive, pthread_mutex_t* workers_mutex)
{
    int ret = pthread_mutex_lock(mutex);
    if (ret == 0)
        return;
    if (ret == EOWNERDEAD)
    {
        pthread_mutex_consistent(mutex);
        printf("[%d] Found a dead body in aisle %d\n", getpid(), shelf_idx);
        pthread_mutex_lock(workers_mutex);
        (*workers_alive)--;
        pthread_mutex_unlock(workers_mutex);
        return;
    }
    errno = ret;
    ERR("pthread_mutex_lock");
}

void child_work(int* tab, pthread_mutex_t* mutexes, int N, pthread_mutex_t* work_mutex, int* work, int* workers_alive, pthread_mutex_t* workers_mutex)
{
    srand(time(NULL) * getpid());
    printf("[%d] Worker reports for a night shift.\n", getpid());

    while (1)
    {
        pthread_mutex_lock(work_mutex);
        if (!(*work))
        {
            pthread_mutex_unlock(work_mutex);
            break;
        }
        pthread_mutex_unlock(work_mutex);
        
        int i = rand() % (N - 1), j = i + 1 + (rand() % (N - i - 1));

        safe_lock_mutex(&mutexes[i], i, workers_alive, workers_mutex);
        safe_lock_mutex(&mutexes[j], j, workers_alive, workers_mutex);

        if ((rand() % 100) == 0)
        {
            printf("[%d] Trips over a pallet and dies\n", getpid());
            abort();
        }

        if (tab[i] > tab[j])
        {
            printf("[%d] Swap %d: %d with %d: %d\n", getpid(), i, tab[i], j, tab[j]);
            int _t = tab[i];
            tab[i] = tab[j];
            tab[j] = _t;
            ms_sleep(100);
        }

        pthread_mutex_unlock(&mutexes[j]);
        pthread_mutex_unlock(&mutexes[i]);
    }

    if (munmap(tab, N * sizeof(int)))
        ERR("munmap");
    if (munmap(mutexes, (N + 2) * sizeof(pthread_mutex_t) + 2 * sizeof(int)))
        ERR("munmap");
    exit(EXIT_SUCCESS);
}

void manager_work(int* tab, pthread_mutex_t* mutexes, int N, pthread_mutex_t* work_mutex, int* work, int* workers_alive, pthread_mutex_t* workers_mutex)
{
    printf("[%d] Manager reports for a night shift.\n", getpid());

    while (1)
    {
        ms_sleep(500);

        for (int i=0;i<N;++i)
            safe_lock_mutex(&mutexes[i], i, workers_alive, workers_mutex);

        print_array(tab, N);

        if (msync(tab, N * sizeof(int), MS_SYNC))
            ERR("msync");

        int sorted = 1;
        for (int i=0;i<N;++i)
        {
            if (tab[i] != i+1)
            {
                sorted = 0;
                break;
            }
        }

        for (int i=N-1;i>=0;--i)
            pthread_mutex_unlock(&mutexes[i]);

        if (sorted)
        {
            printf("[%d] The shop shelves are sorted\n", getpid());
            pthread_mutex_lock(work_mutex);
            *work = 0;
            pthread_mutex_unlock(work_mutex);
            break;
        }

        int should_exit = 0;
        pthread_mutex_lock(workers_mutex);
        printf("[%d] Workers alive: %d\n", getpid(), *workers_alive / 2);
        if (*workers_alive == 0)
            should_exit = 1;
        pthread_mutex_unlock(workers_mutex);

        if (should_exit)
        {
            printf("[%d] All workers died, I hate my job\n", getpid());
            pthread_mutex_lock(work_mutex);
            *work = 0;
            pthread_mutex_unlock(work_mutex);
            break;
        }
    }

    if (munmap(tab, N * sizeof(int)))
        ERR("munmap");
    if (munmap(mutexes, (N + 2) * sizeof(pthread_mutex_t) + 2 * sizeof(int)))
        ERR("munmap");
    exit(EXIT_SUCCESS);
}

void init_mutexes(pthread_mutex_t* mutexes, int N)
{
    for (int i=0;i<N;++i)
    {
        pthread_mutexattr_t mutex_attr;
        pthread_mutexattr_init(&mutex_attr);
        pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
        pthread_mutexattr_setrobust(&mutex_attr, PTHREAD_MUTEX_ROBUST);
        pthread_mutex_init(&mutexes[i], &mutex_attr);
    }
}

int main(int argc, char** argv) 
{ 
    srand(time(NULL));
    if (argc != 3)
    {
        usage(argv[0]);
    }

    int N = atoi(argv[1]), M = atoi(argv[2]);
    if (N < 8 || N > 256 || M < 1 || M > 64)
        usage(argv[0]);

    int fd;
    if ((fd = open(SHOP_FILENAME, O_RDWR | O_CREAT | O_TRUNC, 0666)) < 0)
        ERR("open");
    if (ftruncate(fd, N * sizeof(int)))
        ERR("ftruncate");

    int* tab;
    if ((tab = mmap(NULL, N * sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED)
        ERR("mmap");
    if (close(fd))
        ERR("close");

    void* anon_mem;
    if ((anon_mem = mmap(NULL, (N + 2) * sizeof(pthread_mutex_t) + 2 * sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0)) == MAP_FAILED)
        ERR("mmap");

    pthread_mutex_t* mutexes = anon_mem;
    pthread_mutex_t* work_mutex = mutexes + N;
    pthread_mutex_t* workers_mutex = work_mutex + 1;
    int* work = (int*)(workers_mutex + 1);
    int* workers_alive = work + 1;
    *workers_alive = 2 * M;
    *work = 1;
    init_mutexes(mutexes, N + 2);
    
    for (int i=0;i<N;++i)
        tab[i] = i + 1;

    shuffle(tab, N);

    print_array(tab, N);

    for (int i=0;i<M;++i)
    {
        pid_t pid = fork();
        if (pid < 0)
            ERR("fork");
        if (pid == 0)
            child_work(tab, mutexes, N, work_mutex, work, workers_alive, workers_mutex);
    }

    pid_t mpid = fork();
    if (mpid < 0)
        ERR("fork");
    if (mpid == 0)
        manager_work(tab, mutexes, N, work_mutex, work, workers_alive, workers_mutex);

    while (wait(NULL) > 0)
        ;

    print_array(tab, N);
    printf("Night shift in Bitronka is over\n");

    for (int i=0;i<N+2;++i)
        if (pthread_mutex_destroy(&mutexes[i]))
            ERR("pthread_mutex_destroy");

    if (munmap(anon_mem, (N + 2) * sizeof(pthread_mutex_t) + 2 * sizeof(int)))
        ERR("munmap");
    if (msync(tab, N * sizeof(int), MS_SYNC))
        ERR("msync");
    if (munmap(tab, N * sizeof(int)))
        ERR("munmap");

    return EXIT_SUCCESS;
}
