#include "hw2_output.c"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <semaphore.h>
#include <sys/sem.h>
#include <vector>
using namespace std;
int main(void){
    hw2_init_notifier();
    vector<pthread_t> thread_list;
}