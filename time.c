#include "types.h"
#include "stat.h"
#include "user.h"
#include "fs.h"

int main(int argc, char** argv)
{
    // if(argc<=1)
    // {
    //     printf(2, "time: Insufficient number of arguments\n");
    //     exit();
    // }

    int pid = fork();
    if(pid < 0)
    {
        printf(2, "time: Unable to fork. Exiting..\n");
        exit();
    }
    else if(pid == 0)
    {
        if(argc == 1)
        {
            printf(1, "Running default time function\n");
            for(int i=0;i<100000000;i++)
                ;
            exit();
        }
        else
        {
            printf(1, "Timing %s\n", argv[1]);
            if (exec(argv[1], argv + 1) < 0) 
            {
                printf(2, "exec failed\n");
                exit();
            }
        }
        
    }
    else if(pid > 0)
    {
        int wtime, rtime;
        int id = waitx(&wtime, &rtime);
        if(argc == 1)
        {
            printf(1, "Details of default time function\nWaiting time: %d\nRunning time: %d\n", wtime, rtime);
        }
        else
        {
            printf(1, "Details of time for %s\nProcess id: %d\nWaiting time: %d\nRunning time: %d\n", argv[1], id, wtime, rtime);
        }
        
        exit();
    }
}