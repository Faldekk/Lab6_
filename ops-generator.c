#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define CHANNEL_SIZE 4096

#define CHANNEL_UNINITIALIZED 0
#define CHANNEL_EMPTY 1
#define CHANNEL_OCCUPIED 2
#define CHANNEL_DEPLETED 3

typedef struct
{
    int status;
    pthread_cond_t consumer_cv;
    pthread_cond_t producer_cv;
    pthread_mutex_t mutex;
    char data[CHANNEL_SIZE];
} Channel;

Channel* channel_open(const char* name)
{
    int fd;
    Channel* ch;
    sem_t* init_sem;
    char sem_name[256];

    snprintf(sem_name, sizeof(sem_name), "/init_%s", name);

    init_sem = sem_open(sem_name, O_CREAT, 0666, 1);
    if (init_sem == SEM_FAILED)
    {
        perror("sem_open");
        exit(EXIT_FAILURE);
    }

    sem_wait(init_sem);

    fd = shm_open(name, O_CREAT | O_RDWR, 0666);
    if (fd == -1)
    {
        perror("shm_open");
        exit(EXIT_FAILURE);
    }

    if (ftruncate(fd, sizeof(Channel)) == -1)
    {
        perror("ftruncate");
        exit(EXIT_FAILURE);
    }

    ch = (Channel*)mmap(NULL, sizeof(Channel), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ch == MAP_FAILED)
    {
        perror("mmap");
        exit(EXIT_FAILURE);
    }

    if (close(fd) == -1)
    {
        perror("close");
        exit(EXIT_FAILURE);
    }

    if (ch->status == CHANNEL_UNINITIALIZED)
    {
        pthread_mutexattr_t mattr;
        pthread_condattr_t cattr;

        if (pthread_mutexattr_init(&mattr))
            exit(EXIT_FAILURE);
        if (pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED))
            exit(EXIT_FAILURE);
        if (pthread_mutex_init(&ch->mutex, &mattr))
            exit(EXIT_FAILURE);
        if (pthread_mutexattr_destroy(&mattr))
            exit(EXIT_FAILURE);

        if (pthread_condattr_init(&cattr))
            exit(EXIT_FAILURE);
        if (pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED))
            exit(EXIT_FAILURE);
        if (pthread_cond_init(&ch->consumer_cv, &cattr))
            exit(EXIT_FAILURE);
        if (pthread_cond_init(&ch->producer_cv, &cattr))
            exit(EXIT_FAILURE);
        if (pthread_condattr_destroy(&cattr))
            exit(EXIT_FAILURE);

        ch->status = CHANNEL_EMPTY;
    }

    sem_post(init_sem);
    sem_close(init_sem);

    return ch;
}

void channel_produce(Channel* ch, const char* buffer)
{
    pthread_mutex_lock(&ch->mutex);

    while (ch->status == CHANNEL_OCCUPIED)
        pthread_cond_wait(&ch->producer_cv, &ch->mutex);

    if (ch->status != CHANNEL_DEPLETED)
    {
        strncpy(ch->data, buffer, CHANNEL_SIZE - 1);
        ch->data[CHANNEL_SIZE - 1] = '\0';
        ch->status = CHANNEL_OCCUPIED;
        pthread_cond_signal(&ch->consumer_cv);
    }

    pthread_mutex_unlock(&ch->mutex);
}

void channel_mark_depleted(Channel* ch)
{
    pthread_mutex_lock(&ch->mutex);

    while (ch->status == CHANNEL_OCCUPIED)
        pthread_cond_wait(&ch->producer_cv, &ch->mutex);

    ch->status = CHANNEL_DEPLETED;
    pthread_cond_broadcast(&ch->consumer_cv);

    pthread_mutex_unlock(&ch->mutex);
}

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s file channel\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char* filename = argv[1];
    const char* channel_name = argv[2];

    FILE* file = fopen(filename, "r");
    if (!file)
    {
        perror("fopen");
        exit(EXIT_FAILURE);
    }

    Channel* ch = channel_open(channel_name);

    char line[CHANNEL_SIZE];
    while (fgets(line, sizeof(line), file))
    {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';

        channel_produce(ch, line);
    }

    channel_mark_depleted(ch);

    if (fclose(file) == EOF)
    {
        perror("fclose");
        exit(EXIT_FAILURE);
    }

    return EXIT_SUCCESS;
}
