#include "types.h"
#include "stat.h"
#include "user.h"
#include "fs.h"

int main(int argc, char** argv)
{
    if(argc<=2)
    {
        printf(2, "time: Insufficient number of arguments\n");
        exit();
    }
    if(set_priority(atoi(argv[1]), atoi(argv[2])) == -1)
    {
        printf(1, "An error occured\n");
    }
    exit();
}