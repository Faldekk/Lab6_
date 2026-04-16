#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
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

#define ERR(source) \
    (fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), perror(source), exit(EXIT_FAILURE))

typedef struct
{
    int status;
    pthread_cond_t consumer_cv;
    pthread_cond_t producer_cv;
    pthread_mutex_t mutex;
    char data[CHANNEL_SIZE];
} Channel;

// Otwiera kanał w shared memory, inicjalizuje go tylko przy pierwszym utworzeniu.
// shm_open tworzy lub otwiera obiekt, ftruncate ustawia jego rozmiar, a mmap daje
// zwykły wskaźnik do pamięci współdzielonej widocznej dla wszystkich procesów.
Channel* channel_open(const char* name)
{
    int fd;
    Channel* ch;
    sem_t* init_sem;
    char sem_name[256];

    snprintf(sem_name, sizeof(sem_name), "/init_%s", name);

    // Named semaphore blokuje wyścig między procesami otwierającymi kanał.
    init_sem = sem_open(sem_name, O_CREAT, 0666, 1);
    if (init_sem == SEM_FAILED)
        ERR("sem_open");

    sem_wait(init_sem);

    fd = shm_open(name, O_CREAT | O_RDWR, 0666);
    if (fd == -1)
        ERR("shm_open");

    // Shared memory musi mieć dokładnie rozmiar struktury kanału.
    if (ftruncate(fd, sizeof(Channel)) == -1)
        ERR("ftruncate");

    // mmap mapuje cały kanał do przestrzeni adresowej procesu.
    ch = (Channel*)mmap(NULL, sizeof(Channel), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ch == MAP_FAILED)
        ERR("mmap");

    if (close(fd) == -1)
        ERR("close");

    if (ch->status == CHANNEL_UNINITIALIZED)
    {
        pthread_mutexattr_t mattr;
        pthread_condattr_t cattr;

        // Mutex i condition variable muszą działać między procesami.
        if (pthread_mutexattr_init(&mattr))
            ERR("pthread_mutexattr_init");
        if (pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED))
            ERR("pthread_mutexattr_setpshared");
        if (pthread_mutex_init(&ch->mutex, &mattr))
            ERR("pthread_mutex_init");
        if (pthread_mutexattr_destroy(&mattr))
            ERR("pthread_mutexattr_destroy");

        if (pthread_condattr_init(&cattr))
            ERR("pthread_condattr_init");
        if (pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED))
            ERR("pthread_condattr_setpshared");
        if (pthread_cond_init(&ch->consumer_cv, &cattr))
            ERR("pthread_cond_init");
        if (pthread_cond_init(&ch->producer_cv, &cattr))
            ERR("pthread_cond_init");
        if (pthread_condattr_destroy(&cattr))
            ERR("pthread_condattr_destroy");

        ch->status = CHANNEL_EMPTY;
    }

    // Zwalniamy blokadę inicjalizacji dla kolejnych procesów.
    sem_post(init_sem);
    sem_close(init_sem);

    return ch;
}

// Zamyka lokalne mapowanie kanału.
int channel_close(const char* name, Channel* ch)
{
    if (munmap(ch, sizeof(Channel)))
        ERR("munmap");

    return 0;
}

// Czeka aż w kanale pojawią się dane, kopiuje je do bufora i zwalnia miejsce
// dla producenta. Gdy kanał jest depleted, zwraca 1.
int channel_consume(Channel* ch, char* buffer)
{
    pthread_mutex_lock(&ch->mutex);

    // Konsument zasypia, dopóki producent nie wstawi danych.
    while (ch->status == CHANNEL_EMPTY)
        pthread_cond_wait(&ch->consumer_cv, &ch->mutex);

    if (ch->status == CHANNEL_DEPLETED)
    {
        pthread_mutex_unlock(&ch->mutex);
        return 1;
    }

    // Po odczycie oznaczamy kanał jako pusty i budzimy producenta.
    strcpy(buffer, ch->data);
    ch->status = CHANNEL_EMPTY;
    pthread_cond_signal(&ch->producer_cv);

    pthread_mutex_unlock(&ch->mutex);
    return 0;
}

// Wkłada tekst do kanału. Jeśli kanał jest zajęty, producent czeka.
void channel_produce(Channel* ch, const char* buffer)
{
    pthread_mutex_lock(&ch->mutex);

    // Producent nie nadpisuje danych, dopóki konsument ich nie zabierze.
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

// Oznacza kanał jako pusty na zawsze i budzi wszystkie czekające procesy.
void channel_mark_depleted(Channel* ch)
{
    pthread_mutex_lock(&ch->mutex);

    while (ch->status == CHANNEL_OCCUPIED)
        pthread_cond_wait(&ch->producer_cv, &ch->mutex);

    // Po zakończeniu wejścia dalsze consume mają wiedzieć, że nic już nie będzie.
    ch->status = CHANNEL_DEPLETED;
    pthread_cond_broadcast(&ch->consumer_cv);

    pthread_mutex_unlock(&ch->mutex);
}

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s input_channel output_channel\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char* input_name = argv[1];
    const char* output_name = argv[2];

    Channel* input_ch = channel_open(input_name);
    Channel* output_ch = channel_open(output_name);

    char buffer[CHANNEL_SIZE];
    char output_buffer[CHANNEL_SIZE * 2];

    while (1)
    {
        // Odbiór z kanału wejściowego blokuje się, gdy nie ma jeszcze danych.
        if (channel_consume(input_ch, buffer))
            break;

        // To jest etap pośredni: pokazujemy wejście i budujemy zduplikowany tekst.
        printf("%s\n", buffer);
        fflush(stdout);

        char* src = buffer;
        char* dst = output_buffer;
        size_t dstlen = 0;

        // Każdy znak z wejścia trafia dwa razy do bufora wyjściowego.
        while (*src && dstlen < CHANNEL_SIZE - 1)
        {
            *dst++ = *src;
            dstlen++;
            if (dstlen < CHANNEL_SIZE - 1)
            {
                *dst++ = *src;
                dstlen++;
            }
            src++;
        }
        *dst = '\0';

        // Jeśli duplikacja nie mieści się w jednym komunikacie, dzielimy ją na części.
        if (dstlen > CHANNEL_SIZE - 1)
        {
            char part1[CHANNEL_SIZE];
            char part2[CHANNEL_SIZE];
            strncpy(part1, output_buffer, CHANNEL_SIZE - 1);
            part1[CHANNEL_SIZE - 1] = '\0';
            strcpy(part2, output_buffer + strlen(part1));

            channel_produce(output_ch, part1);
            channel_produce(output_ch, part2);
        }
        else
        {
            channel_produce(output_ch, output_buffer);
        }
    }

    channel_mark_depleted(output_ch);

    channel_close(input_name, input_ch);
    channel_close(output_name, output_ch);

    return EXIT_SUCCESS;
}
