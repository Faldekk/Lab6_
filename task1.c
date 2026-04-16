#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHAR_COUNT 256

#define ERR(source) \
    (fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), perror(source), kill(0, SIGKILL), exit(EXIT_FAILURE))

void usage(char* name)
{
    fprintf(stderr, "USAGE: %s file N\n", name);
    fprintf(stderr, "N > 0 - number of child processes\n");
    exit(EXIT_FAILURE);
}

size_t get_file_size(const char* filename)
{
    int fd;
    struct stat sb;

    if ((fd = open(filename, O_RDONLY)) == -1)
        ERR("open");
    if (fstat(fd, &sb) == -1)
        ERR("fstat");
    if (close(fd) == -1)
        ERR("close");

    if (sb.st_size <= 0)
    {
        fprintf(stderr, "File is empty\n");
        exit(EXIT_FAILURE);
    }

    return (size_t)sb.st_size;
}

void print_file_with_mmap(const char* filename, size_t file_size)
{
    int fd;
    char* data;

    if ((fd = open(filename, O_RDONLY)) == -1)
        ERR("open");

    if ((data = (char*)mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0)) == MAP_FAILED)
        ERR("mmap");

    if (write(STDOUT_FILENO, data, file_size) == -1)
        ERR("write");
    if (write(STDOUT_FILENO, "\n", 1) == -1)
        ERR("write");

    if (munmap(data, file_size))
        ERR("munmap");
    if (close(fd) == -1)
        ERR("close");
}

void child_work(const char* filename, size_t file_size, int id, int n, long* shared_counts)
{
    int fd;
    char* data;
    size_t chunk_size;
    size_t start;
    size_t end;
    long local_counts[CHAR_COUNT];

    memset(local_counts, 0, sizeof(local_counts));
    srand((unsigned int)getpid());

    if ((fd = open(filename, O_RDONLY)) == -1)
        ERR("open");

    if ((data = (char*)mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0)) == MAP_FAILED)
        ERR("mmap");

    chunk_size = file_size / (size_t)n;
    start = (size_t)id * chunk_size;
    end = (id == n - 1) ? file_size : start + chunk_size;

    for (size_t i = start; i < end; i++)
    {
        unsigned char c = (unsigned char)data[i];
        local_counts[c]++;
    }

    if ((rand() % 100) < 3)
        abort();

    for (int i = 0; i < CHAR_COUNT; i++)
    {
        shared_counts[id * CHAR_COUNT + i] = local_counts[i];
    }

    if (munmap(data, file_size))
        ERR("munmap");
    if (close(fd) == -1)
        ERR("close");
}

int wait_for_children(int n)
{
    int ok = 1;

    for (int i = 0; i < n; i++)
    {
        int status;
        pid_t pid = wait(&status);
        if (pid <= 0)
        {
            ok = 0;
            continue;
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            ok = 0;
    }

    return ok;
}

void print_summary(long* shared_counts, int n)
{
    long total[CHAR_COUNT];
    memset(total, 0, sizeof(total));

    for (int p = 0; p < n; p++)
    {
        for (int i = 0; i < CHAR_COUNT; i++)
            total[i] += shared_counts[p * CHAR_COUNT + i];
    }

    printf("Character counts:\n");
    for (int i = 0; i < CHAR_COUNT; i++)
    {
        if (total[i] > 0)
        {
            if (i >= 32 && i <= 126)
                printf("'%c': %ld\n", (char)i, total[i]);
            else
                printf("[%d]: %ld\n", i, total[i]);
        }
    }
}

int main(int argc, char** argv)
{
    if (argc != 3)
        usage(argv[0]);

    const char* filename = argv[1];
    int n = atoi(argv[2]);
    if (n <= 0)
        usage(argv[0]);

    size_t file_size = get_file_size(filename);
    print_file_with_mmap(filename, file_size);

    long* shared_counts;
    size_t shared_size = (size_t)n * CHAR_COUNT * sizeof(long);
    if ((shared_counts = (long*)mmap(NULL, shared_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0)) ==
        MAP_FAILED)
        ERR("mmap");
    memset(shared_counts, 0, shared_size);

    for (int i = 0; i < n; i++)
    {
        switch (fork())
        {
            case 0:
                child_work(filename, file_size, i, n, shared_counts);
                exit(EXIT_SUCCESS);
            case -1:
                ERR("fork");
        }
    }

    if (wait_for_children(n))
        print_summary(shared_counts, n);
    else
        printf("Computation failed: a child process terminated unexpectedly.\n");

    if (munmap(shared_counts, shared_size))
        ERR("munmap");

    return EXIT_SUCCESS;
}