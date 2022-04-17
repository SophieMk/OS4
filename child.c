#define _POSIX_SOURCE // for kill

#include <assert.h>
#include <fcntl.h>
#include <float.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

bool ready(pid_t pid_parent) {
    if (kill(pid_parent, SIGUSR1) == -1) { // говорим, что готовы ждать запрос
        perror("kill");
        return false;
    }
    return true;
}

int main(int argc, char* argv[]) { //принимает на вход вспомогательный файл и результирующий
    fprintf(stderr, "DEBUG: child: started\n");

    if (argc != 3) {
        return 1;
    }
    char* filename_map = argv[1]; //вспомогательный файл
    char* filename_result = argv[2]; //результирующий файл

    int fd_map = open(filename_map, O_RDWR, 0600); //открываем файл
    if (fd_map == -1) {
        perror(filename_map);
        return 1;
    }

    size_t n_numbers_max = 100; 
    size_t length_map = (n_numbers_max + 1) * sizeof(float);
    //вспомогательный файл отображаем на область памяти
    float* map = mmap(NULL, length_map, PROT_READ | PROT_WRITE, MAP_SHARED, fd_map, 0);
    if (map == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    FILE* file_result = fopen(filename_result, "w"); //открываем файл с результатами
    //через fopen, тк при записи результатов мы можем их кэшировать, чтобы сразу на 
    //жёсткий диск не писать
    if (file_result == NULL) {
        perror(filename_result);
        return 1;
    }

    pid_t pid_parent = getppid(); //узнаём номер родителя, чтобы ему отправлять сигнал
    fprintf(stderr, "DEBUG: child: pid_parent=%d\n", pid_parent);

    bool is_ok = true;
    bool is_done = false;
    while (! is_done) {
        fprintf(stderr, "DEBUG: child: trying to lock\n");
        flock(fd_map, LOCK_EX); //захватываем файл
        fprintf(stderr, "DEBUG: child: locked\n");

        int k_numbers = (int) map[0];//смотрим, что в нулевом служебном элементе
        if (k_numbers == -1) { //команды закончилимь
            is_done = true;
        } else if (k_numbers > 0) { //суммируем
            float sum = 0;
            for (int i_number = 1; i_number <= k_numbers; i_number++) {
                sum += map[i_number];
            }

            if (fprintf(file_result, "%f\n", sum) < 0) {
                printf("fprintf error\n");
                is_ok = false;
                is_done = true;
            }

            map[0] = 0; // чтобы ребёнок не выполнял ту же команду снова
            msync(map, sizeof(float), MS_SYNC);
        }

        flock(fd_map, LOCK_UN); //разблокируем файл
        fprintf(stderr, "DEBUG: child: unlocked\n");

        ready(pid_parent); //сообщаем, что готовы
    }

    if (munmap(map, length_map) == -1) { //закрываем отображение файла на память
        perror("munmap");
        is_ok = false;
    }

    if (close(fd_map) == -1) { //закрываем сам файл
        perror(filename_map);
        is_ok = false;
    }

    if (fclose(file_result) != 0) { //закрываем результирующий файл
        printf("fclose error\n");
        is_ok = false;
    }

    return is_ok ? 0 : 1;
}
