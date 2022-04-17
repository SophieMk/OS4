#define  _GNU_SOURCE  // for getline

#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <unistd.h>

bool is_child_ready = false;

void sig_handler(int signum) { //обработчик сигнала
    is_child_ready = true;
}

char* read_filename() { //считываем имя файла
    char* line = NULL;
    size_t bufsize = 0;
    int nread = getline(&line, &bufsize, stdin);
    if (nread == -1) {
        return NULL; 
    }
    size_t len = strlen(line);
    line[len - 1] = '\0';
    return line; 
}

/*
bool wait_for_child_readiness() {
    sigset_t set;

    sigemptyset(&set);
    if (sigaddset(&set, SIGUSR1) == -1) {
        perror("sigaddset");
        return false;
    }

    int sig;
    if (sigwait(&set, &sig) != 0) {
        fprintf(stderr, "sigwait error\n");
        return false;
    }

    return true;
}
*/

bool translate_stdin_to_map(int fd_map, float* map, size_t n_numbers_max) {
    fprintf(stderr, "DEBUG: translate_stdin_to_map: started\n");

    bool is_ok = true;
    bool is_done = false;

    while (! is_done) { // для каждой команды:
        flock(fd_map, LOCK_EX); //"захватываем" файл
        fprintf(stderr, "DEBUG: locked\n");

        is_child_ready = false; //выставляем, что ребёнок не готов принять текущую команду

        fprintf(stderr, "DEBUG: --- reading a new command ---\n");
        for (size_t i_number = 1; ; i_number++) { //считываем команду
            fprintf(stderr, "DEBUG: i_number: %zu\n", i_number);

            if (i_number > n_numbers_max) { //слишком много чисел для выделенной памяти
                fprintf(stderr, "too many numbers\n");
                is_ok = false;
                is_done = true;
                break; //выходим из цикла
            }

            // Считываем очередное число из входного файла и преобразуем его из текста во float.
            float number;
            int result_scanf = scanf("%f", &number);
            if (result_scanf == EOF) { // встретили конец файла
                fprintf(stderr, "DEBUG: EOF\n");
                is_ok = i_number == 1; 
                is_done = true; //если начали читать новую команду, а там вначале конец файла, то всё хорошо
                break;
            }
            if (result_scanf == 0) { //если там не число
                fprintf(stderr, "expected a number\n");
                is_ok = false;
                is_done = true;
                break;
            }
            assert(result_scanf == 1); //проверяем, что одно число считалось и сохраняем это число 

            fprintf(stderr, "DEBUG: number: %f\n", number);
            map[i_number] = number; //записываем это число

            char sep = getchar();
            fprintf(stderr, "DEBUG: sep: %02x\n", sep);
            if (sep != '\n') { //если это не символ новой строки, продолжаем
                continue;
            }

            // Записываем числа команды:
            assert(i_number == (float) i_number); // представимо точно (проверяем, что можем записать 
                                                  //максимальное количество чисел в точном представлении float)
            map[0] = i_number;
            msync(map, (i_number + 1) * sizeof(float), MS_SYNC); //синхронизация памяти  
            fprintf(stderr, "DEBUG: msynced\n");
            break; // переходим к новом команде
        }

        if (is_done) { 
            map[0] = -1; //служебное число
            msync(map, sizeof(float), MS_SYNC);
        }

        flock(fd_map, LOCK_UN); //разблокируем файл
        fprintf(stderr, "DEBUG: unlocked\n");

        while (! is_child_ready) { //ждём, пока ребёнок не завершится
            sleep(1);
        }
        fprintf(stderr, "DEBUG: child is ready\n");
    }

    return is_ok;
}

bool wait_for_child_exit() {
    int wstatus;
    if (wait(&wstatus) == -1) {
        perror("wait");
        return false;
    }
    // Ребёнок завершился.
    if (! (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0)) {
        fprintf(stderr, "error in child\n");
        return false;
    }

    return true;
}

int main(int argc, char *argv[]) {
    if (argc != 2) { // arg count
        fprintf(stderr, "Usage: %s MAPFILE\n", argv[0]);
        return 1;
    }

    char* filename_map = argv[1];

    //open(имя файла, права чтение&запись | создаём файл | если файл был создан, обрезаем, права доступа)
    // 0600 =  rwx rwx rwx

    int fd_map = open(filename_map, O_RDWR | O_CREAT | O_TRUNC, 0600); //открываем файл
    if (fd_map == -1) { //если не получилось, то выходим
        perror(filename_map); // глобальная переменная errno(что именно произошло), perror смотрит,
                              //что написано в errno и отображает соответствующее сообщение
        return 1;
    }
    flock(fd_map, LOCK_EX); // блокируем файл, чтобы ребёнок пока не читал

    char* filename_result = read_filename();
    if (filename_result == NULL) { //если есть ошибка
        fprintf(stderr, "expected a filename\n"); //выводим ошибку на стандартный поток ошибок
        return 1;
    }
    fprintf(stderr, "DEBUG: filename_result: %s\n", filename_result);

    signal(SIGUSR1, sig_handler); //сигнал о том, что ребёнок готов, вызывается функция sig_handler

    //создаём ребёнка
    int pid_child = fork();
    if (pid_child == -1) { //если не вышло, вывели сообщение об ошибке
        perror("fork");
        return 1;
    }
    // pid_child=0 -- у дочернего процесса
    // pid_child=номер_дочернего -- у родительского процесса

    if (pid_child == 0) { // Мы в дочернем процессе.
        //закрываем файл, чтобы в ребёнке его снова открыть
        if (close(fd_map) == -1) {
            perror(filename_map);
            return 1;
        }
        //запускаем ребёнка, передаём имя вспомогательного файла и куда записывать результаты 
        char* argv_child[] = {"./child", filename_map, filename_result, NULL};
        if (execv("child", argv_child) == -1) { //эта функция не должна завершиться, если 
                                                //завершилась, выводим ошибку
            perror("execv");
        }
        return 1; // Если вдруг вернулись из функции execv.
    }

    // Мы в родительском процессе.
    assert(pid_child > 0);

    size_t n_numbers_max = 100; //максимум чисел, которые будем хранить
    size_t length_map = (n_numbers_max + 1) * sizeof(float); //расширяем файл, чтобы эти числа туда влезли
    //(сколько байт)
    //забиваем файл нулями
    //fallocate(fd, int mod, off_t offset, off_t, len);
    if (fallocate(fd_map, 0, 0, length_map) == -1) {
        perror("fallocate");
        return 1;
    }

    //файл отображаем на массив, с которым дальще будем работать
    float* map = mmap(NULL, length_map, PROT_WRITE, MAP_SHARED, fd_map, 0); 
    //mmap(то, на какой адрес надо отобразить, сколько байт, будем обращаться на запись, 
    //те изменения, которые мы сделаем, должны быть и в самом файле, что это за файл, смещение(с начала)); 
    if (map == MAP_FAILED) { //если не удалось, выходим
        perror("mmap");
        return 1;
    }
    fprintf(stderr, "DEBUG: map: %p\n", map);

    bool is_ok = true;

    if (! translate_stdin_to_map(fd_map, map, n_numbers_max)) { //запускаем translate_stdin_to_map
        is_ok = false;
    }

    if (munmap(map, length_map) == -1) { //закрываем отображение файла на память
        perror("munmap");
        is_ok = false;
    }

    if (close(fd_map) == -1) { //закрываем сам файл
        perror(filename_map);
        is_ok = false;
    }

    if (! wait_for_child_exit()) {//ждём, когда ребёнок выйдет
        is_ok = false;
    }

    return is_ok ? 0 : 1;
}
