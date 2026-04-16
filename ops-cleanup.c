#define _GNU_SOURCE

#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s channel_name [channel_name ...]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    for (int i = 1; i < argc; i++)
    {
        const char* name = argv[i];
        char sem_name[256];

        // Usuwamy obiekt shared memory przypisany do kanału.
        if (shm_unlink(name) == -1)
            perror("shm_unlink");
        else
            printf("Unlinked shared memory: %s\n", name);

        // Usuwamy nazwany semafor używany do inicjalizacji kanału.
        snprintf(sem_name, sizeof(sem_name), "/init_%s", name);
        if (sem_unlink(sem_name) == -1)
            perror("sem_unlink");
        else
            printf("Unlinked semaphore: %s\n", sem_name);
    }

    return EXIT_SUCCESS;
}
