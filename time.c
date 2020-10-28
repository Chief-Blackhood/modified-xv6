#include "types.h"
#include "stat.h"
#include "user.h"
#include "fs.h"

int main(int argc, char** argv)
{
    if(argc<=1)
    {
        printf(2, "time: Insufficient number of arguments\n");
        exit();
    }

    int pid = fork();
    if(pid < 0)
    {
        printf(2, "time: Unable to fork. Exiting..\n");
        exit();
    }
    else if(pid == 0)
    {
        if (exec(argv[1], argv + 1) < 0) 
        {
            printf(2, "exec failed\n");
            exit();
        }
    }
    else if(pid > 0)
    {
        int wtime, rtime;
        int wid = waitx(&wtime, &rtime);
        printf(1, "Details of time for %s\nProcess id: %d\nWaiting time: %d\nRunning time: %d\n", argv[1], wid, wtime, rtime);
        exit();
    }
}