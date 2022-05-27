extern "C" {
#include "hw2_output.h"
}
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <iostream>
#include <pthread.h>
#include <sys/time.h>
#include <semaphore.h>
#include <vector>
#include <tuple>
using namespace std;

int **matrix;
int **smoker_count;
sem_t **semaphores;
vector<pair<int, string>> commands;
pthread_mutex_t cond_mutex_stop_smoking;
pthread_cond_t cond_stop_smoking;
pthread_mutex_t cond_mutex_breakstop;
pthread_cond_t cond_var_breakstop;
pthread_mutex_t cond_mutex_continuestop;
pthread_cond_t cond_var_continuestop;
pthread_cond_t wake_up_cond;
pthread_mutex_t wake_up_mutex;
pthread_mutex_t waiting_mutex;
pthread_mutex_t stop_mutex;
pthread_mutex_t lock_mutex;
pthread_mutex_t smoker_count_mutex;
pthread_mutex_t start_mutex;
pthread_cond_t start_cond;
bool is_break = false;
bool is_stop = false;
int priv_num;
int smoker_num = -1;
int waiting = 0;
int stopped = 0;

struct privates{
    int gid;
    int si; //start index x
    int sj; //start index y
    int tg; //time of cleaning
    int ng; //number of groups
    vector<pair<int, int>> *start_points;
};

struct sneaky_smoker{
    int sid;
    int ts; //time of smoking
    int cc; //number of cells
    vector<tuple<int, int, int>> *smoke_points;
};

void calculateStopTime(struct timespec *stoptime, struct timeval *timeofday, int sleep_time){
    gettimeofday(timeofday, nullptr);
    stoptime->tv_sec = timeofday->tv_sec;
    stoptime->tv_nsec = timeofday->tv_usec * 1000;
    stoptime->tv_sec += sleep_time / 1000;
    stoptime->tv_nsec += (sleep_time % 1000) * 1000000;
    if(stoptime->tv_nsec >= 1000000000){
        stoptime->tv_nsec -= 1000000000;
        stoptime->tv_sec++;
    }
}

void waitForContinue(privates *priv){
    pthread_mutex_lock(&waiting_mutex);
    waiting++;
    pthread_mutex_unlock(&waiting_mutex);

    hw2_notify(PROPER_PRIVATE_TOOK_BREAK, priv->gid, 0, 0);
    pthread_mutex_lock(&cond_mutex_continuestop);
    pthread_cond_wait(&cond_var_continuestop, &cond_mutex_continuestop);
    pthread_mutex_unlock(&cond_mutex_continuestop);
    if(is_stop){ //stop is given when private is in break
        hw2_notify(PROPER_PRIVATE_STOPPED, priv->gid, 0, 0);
        pthread_mutex_lock(&stop_mutex);
        priv_num--;
        stopped++;
        pthread_mutex_unlock(&stop_mutex);
        pthread_exit(nullptr);
    }

    pthread_mutex_lock(&waiting_mutex);
    waiting--;
    pthread_mutex_unlock(&waiting_mutex);
    hw2_notify(PROPER_PRIVATE_CONTINUED, priv->gid, 0, 0);
}

void waitForWakeUpCleaner(privates *priv){ //privates wait here to be woken up
    pthread_mutex_lock(&wake_up_mutex);
    pthread_cond_wait(&wake_up_cond, &wake_up_mutex); //wake up condition may be signaled by orders or by cleaned areas
    pthread_mutex_unlock(&wake_up_mutex);
    if(is_break){ //break is given while waiting for the area, go to break
        waitForContinue(priv);
    }
    else if(is_stop){ //stop is given while waiting for the area, exit
        hw2_notify(PROPER_PRIVATE_STOPPED, priv->gid, 0, 0);
        pthread_mutex_lock(&stop_mutex);
        stopped++;
        priv_num--;
        pthread_mutex_unlock(&stop_mutex);
        pthread_exit(nullptr);
    }
}

void waitForWakeUpSmoker(sneaky_smoker *smoker){ //privates wait here to be woken up
    pthread_mutex_lock(&wake_up_mutex);
    pthread_cond_wait(&wake_up_cond, &wake_up_mutex); //wake up condition may be signaled by orders or by cleaned areas
    pthread_mutex_unlock(&wake_up_mutex);
    if(is_stop){ //stop is given while waiting for the area, exit
        hw2_notify(SNEAKY_SMOKER_STOPPED, smoker->sid, 0, 0);
        pthread_mutex_lock(&stop_mutex);
        stopped++;
        smoker_num--;
        pthread_mutex_unlock(&stop_mutex);
        pthread_exit(nullptr);
    }
    //else break is given while waiting for the area, go to area and try to lock
}

void *command_func(void *arg) {
    //commander thread
    auto command_num = commands.size();
    for(int i = 0; i < command_num; i++){
        int sleep_time;
        int command_time = commands[i].first;
        string command = commands[i].second;
        if(i == 0){
            sleep_time = command_time;
        }
        else{
            sleep_time = commands[i].first - commands[i-1].first;
        }
        usleep(sleep_time*1000);

        //embed komutanim
        if(command == "break"){
            hw2_notify(ORDER_BREAK, 0, 0, 0);
            is_break = true;
            while(waiting < priv_num){
                pthread_cond_broadcast(&cond_var_breakstop);
                pthread_cond_broadcast(&wake_up_cond);
            }
        }
        else if(command == "stop"){
            int temp_num = priv_num + smoker_num;
            hw2_notify(ORDER_STOP, 0, 0, 0);
            is_break = false;
            is_stop = true;
            while(stopped < temp_num){
                pthread_cond_broadcast(&cond_var_continuestop);
                pthread_cond_broadcast(&cond_var_breakstop);
                pthread_cond_broadcast(&cond_stop_smoking);
                pthread_cond_broadcast(&wake_up_cond);
            }
        }
        else if(command == "continue"){
            hw2_notify(ORDER_CONTINUE, 0, 0, 0);
            is_break = false;
            while(waiting){
                pthread_cond_broadcast(&cond_var_continuestop);
            }
        }
    }
    return nullptr;
}

void *clean(void *arg){
    struct timespec stoptime{};
    struct timeval timeofday{};
    auto *priv = (struct privates *)arg;
    hw2_notify(PROPER_PRIVATE_CREATED, priv->gid, 0, 0);
    int sleep_time = priv->tg; //time of cleaning in ms
    vector<pair<int,int>> *start_points = priv->start_points;

    clean:
    while(!start_points->empty()){
        int start_x = start_points->front().first; //starting point x
        int start_y = start_points->front().second; //starting point y
        int end_x = start_x + priv->si; //ending point x //ending point x
        int end_y = start_y + priv->sj; //ending point y //ending point y
        //try to lock the area
        for(int j = start_x; j < end_x; j++){
            for(int k = start_y; k < end_y; k++){
                if(sem_trywait(&semaphores[j][k]) != 0){ //check if the cell is available
                    for(int l = start_x; l < end_x; l++){ //it is not available, unlock the locked cells
                        for(int m = start_y; m <= end_y; m++){
                            if(l == j && m == k){
                                waitForWakeUpCleaner(priv); //sleep until someone finishes smoking or cleaning
                                goto clean; //start cleaning again
                            }
                            sem_post(&semaphores[l][m]);
                        }
                    }
                }
            }
        }
        //notify that the area is locked
        hw2_notify(PROPER_PRIVATE_ARRIVED, priv->gid, start_x, start_y);
        for(int j = start_x; j < end_x; j++){
            for(int k = start_y; k < end_y; k++){
                while(matrix[j][k]){
                    calculateStopTime(&stoptime, &timeofday, sleep_time);
                    //wait for the time to pass
                    pthread_mutex_lock(&cond_mutex_breakstop);
                    int res = pthread_cond_timedwait(&cond_var_breakstop, &cond_mutex_breakstop, &stoptime);
                    pthread_mutex_unlock(&cond_mutex_breakstop);
                    if(res == 0){ //it is not a timeout, a command is given
                        //unlock the locked area, since a command is given
                        for(int a = start_x; a < end_x; a++){
                            for(int b = start_y; b < end_y; b++){
                                sem_post(&semaphores[a][b]);
                            }
                        }
                        if(is_break){ //command is break
                            waitForContinue(priv);
                            goto clean;
                        }
                        if(is_stop){ //command is stop
                            pthread_mutex_lock(&stop_mutex);
                            stopped++;
                            priv_num--;
                            pthread_mutex_unlock(&stop_mutex);
                            hw2_notify(PROPER_PRIVATE_STOPPED, priv->gid, 0, 0);
                            pthread_exit(nullptr);
                        }
                    }
                    //else it is timed out, just continue
                    matrix[j][k]--;
                    hw2_notify(PROPER_PRIVATE_GATHERED, priv->gid, j, k);
                }
            }
        }
        //unlock the area
        for(int j = start_x; j < end_x; j++){
            for(int k = start_y; k < end_y; k++){
                sem_post(&semaphores[j][k]);
            }
        }

        //delete the point from vector
        start_points->erase(start_points->begin());
        hw2_notify(PROPER_PRIVATE_CLEARED, priv->gid, 0, 0);
        pthread_cond_broadcast(&wake_up_cond); //wake up everyone waiting
    }
    hw2_notify(PROPER_PRIVATE_EXITED, priv->gid, 0, 0);
    pthread_mutex_lock(&stop_mutex);
    priv_num--;
    pthread_mutex_unlock(&stop_mutex);
    return nullptr;
}

void *smoking(void *arg){
    auto *smoker = (struct sneaky_smoker *)arg;
    hw2_notify(SNEAKY_SMOKER_CREATED, smoker->sid, 0, 0);
    struct timespec stoptime{};
    struct timeval timeofday{};
    int smoke_time = smoker->ts; //time of smoking in ms
    vector<tuple<int,int,int>> *smoke_points = smoker->smoke_points;
    smoke:
        while(!smoke_points->empty()){
            int smoke_i = get<0>(smoke_points->front()); //starting point x
            int smoke_j = get<1>(smoke_points->front()); //starting point y
            int cigarette_count = get<2>(smoke_points->front()); //cigarette count
            int start_i = smoke_i - 1;
            int start_j = smoke_j - 1;
            int end_i = smoke_i + 1;
            int end_j = smoke_j + 1;
            int cigar_i = 0;
            int cigar_j = 0;
            int caseNum = 0;
            //try to lock the cell first
            if(sem_trywait(&semaphores[smoke_i][smoke_j]) != 0){ //there shouldn't be smoker or cleaner in this cell
                waitForWakeUpSmoker(smoker); //if there is, goto sleep
                goto smoke;
            }
            pthread_mutex_lock(&smoker_count_mutex);
            smoker_count[smoke_i][smoke_j]++;
            pthread_mutex_unlock(&smoker_count_mutex);
            //if cell is available, try to get the lock of the cells around it
            for(int i = start_i; i <= end_i; i++){
                for(int j = start_j; j <= end_j; j++){
                    if(smoker_count[i][j] > 0 || (i == smoke_i && j == smoke_j)){ //if there is a smoker in this cell, no need to lock it
                        pthread_mutex_lock(&smoker_count_mutex);
                        smoker_count[i][j]++;
                        pthread_mutex_unlock(&smoker_count_mutex);
                        continue;
                    }
                    if(sem_trywait(&semaphores[i][j]) != 0){ //check if the cell is available
                        for(int a = start_i; a <= end_i; a++){ //it is not available, unlock the locked cells
                            for(int b = start_j; b <= end_j; b++){
                                if(a == i && b == j){
                                    waitForWakeUpSmoker(smoker);
                                    goto smoke;
                                }
                                if(a == smoke_i && b == smoke_j){
                                    pthread_mutex_lock(&smoker_count_mutex);
                                    smoker_count[i][j]--;
                                    pthread_mutex_unlock(&smoker_count_mutex);
                                    sem_post(&semaphores[a][b]);
                                }
                                else{
                                    if(smoker_count[a][b] == 1){
                                        sem_post(&semaphores[a][b]);
                                    }
                                    pthread_mutex_lock(&smoker_count_mutex);
                                    smoker_count[i][j]--;
                                    pthread_mutex_unlock(&smoker_count_mutex);
                                }
                            }
                        }
                    }
                    else{ //lock is successful
                        pthread_mutex_lock(&smoker_count_mutex);
                        smoker_count[i][j]++;
                        pthread_mutex_unlock(&smoker_count_mutex);
                    }
                }
            }
            //notify that the area is locked
            hw2_notify(SNEAKY_SMOKER_ARRIVED, smoker->sid, smoke_i, smoke_j);
            //smoke cigarette count times
            for(int i = 0; i < cigarette_count; i++){
                calculateStopTime(&stoptime, &timeofday, smoke_time);
                //wait for the time to pass
                pthread_mutex_lock(&cond_mutex_stop_smoking);
                int res = pthread_cond_timedwait(&cond_stop_smoking, &cond_mutex_stop_smoking, &stoptime);
                pthread_mutex_unlock(&cond_mutex_stop_smoking);
                if(res == 0){ //it is not a timeout, a command is given
                    //unlock the locked area, since a command is given
                    for(int a = start_i; a < end_i; a++){
                        for(int b = start_j; b < end_j; b++){
                            sem_post(&semaphores[a][b]);
                        }
                    }
                    if(is_stop){ //command is stop
                        pthread_mutex_lock(&stop_mutex);
                        stopped++;
                        pthread_mutex_unlock(&stop_mutex);
                        hw2_notify(SNEAKY_SMOKER_STOPPED, smoker->sid, 0, 0);
                        pthread_exit(nullptr);
                    }
                }
                //else it is timed out, just continue
                matrix[start_i+cigar_i][start_j+cigar_j]++;
                hw2_notify(SNEAKY_SMOKER_FLICKED, smoker->sid, cigar_i+start_i, cigar_j+start_j);
                caseNum++;
                switch(caseNum%8){
                    case 0:
                        cigar_i = 0;
                        cigar_j = 0;
                        break;
                    case 1:
                        cigar_i = 0;
                        cigar_j = 1;
                        break;
                    case 2:
                        cigar_i = 0;
                        cigar_j = 2;
                        break;
                    case 3:
                        cigar_i = 1;
                        cigar_j = 2;
                        break;
                    case 4:
                        cigar_i = 2;
                        cigar_j = 2;
                        break;
                    case 5:
                        cigar_i = 2;
                        cigar_j = 1;
                        break;
                    case 6:
                        cigar_i = 2;
                        cigar_j = 0;
                        break;
                    case 7:
                        cigar_i = 1;
                        cigar_j = 0;
                        break;
                }
            }
            //unlock the area
            for(int i = start_i; i <= end_i; i++){
                for(int j = start_j; j <= end_j; j++){
                    if(smoker_count[i][j] == 1 && !(i == smoke_i && j == smoke_j)){
                        sem_post(&semaphores[i][j]);
                    }
                    pthread_mutex_lock(&smoker_count_mutex);
                    smoker_count[i][j]--;
                    pthread_mutex_unlock(&smoker_count_mutex);
                }
            }
            sem_post(&semaphores[smoke_i][smoke_j]);
            //delete the point from vector
            smoke_points->erase(smoke_points->begin());
            hw2_notify(SNEAKY_SMOKER_LEFT, smoker->sid, smoke_i, smoke_j);
            pthread_cond_broadcast(&wake_up_cond); //wake up everyone waiting
        }

    hw2_notify(SNEAKY_SMOKER_EXITED, smoker->sid, 0, 0);
    pthread_mutex_lock(&smoker_count_mutex);
    smoker_num--;
    pthread_mutex_unlock(&smoker_count_mutex);
    return nullptr;
}

int main(){
    hw2_init_notifier();
    vector<privates> priv;
    vector<sneaky_smoker> smoker;
    int grid_x;
    int grid_y;
    int wait_count;

    #pragma region "getting input"
    cin >> grid_x;
    cin >> grid_y;
    //get input matrix from user
    matrix = new int*[grid_x];
    for(int i = 0; i < grid_x; i++){
        matrix[i] = new int[grid_y];
    }
    for(int i = 0; i < grid_x; i++){
        for(int j = 0; j < grid_y; j++){
            cin >> matrix[i][j];
        }
    }
    cin >> priv_num;
    wait_count = priv_num;
    for(int i = 0; i < priv_num; i++){ //create the privates
        privates temp{};
        cin >> temp.gid;
        cin >> temp.si;
        cin >> temp.sj;
        cin >> temp.tg;
        cin >> temp.ng;
        temp.start_points = new vector<pair<int, int>>();
        for(int j = 0; j < temp.ng; j++){
            int temp_x;
            int temp_y;
            cin >> temp_x;
            cin >> temp_y;
            temp.start_points->emplace_back(temp_x,temp_y);
        }
        priv.push_back(temp);
    }
    #pragma endregion

    #pragma region "creating mutexes and is_smoker flags for each cell"
    semaphores = new sem_t*[grid_x];
    smoker_count = new int*[grid_x];
    for(int i = 0; i < grid_x; i++) {
        semaphores[i] = new sem_t[grid_y];
        smoker_count[i] = new int[grid_y];
        for(int j = 0; j < grid_y; j++){
            sem_init(&semaphores[i][j], 0, 1);
            smoker_count[i][j] = 0;
        }
    }

    #pragma endregion

    #pragma region "taking commands"
    int command_num = -1;
    cin >> command_num;
    if(command_num != EOF){
        for(int i = 0; i < command_num; i++){
        string command;
        int time;
        cin >> time;
        cin >> command;
        commands.emplace_back(time,command);
        }
    }
    else{
        command_num = 0;
    }

    #pragma endregion

    #pragma region "taking smoker info"
    cin >> smoker_num;
    int smoker_wait = smoker_num;
    if(smoker_num != EOF){
        for(int i = 0; i < smoker_num; i++){
        struct sneaky_smoker temp{};
        cin >> temp.sid;
        cin >> temp.ts;
        cin >> temp.cc;
        temp.smoke_points = new vector<tuple<int, int, int>>();
            for(int j = 0; j < temp.cc; j++){
                int temp_i;
                int temp_j;
                int count;
                cin >> temp_i;
                cin >> temp_j;
                cin >> count;
                temp.smoke_points->emplace_back(temp_i,temp_j, count);
            }
            smoker.push_back(temp);
        }   
    }
    else{
        smoker_num = 0;
    }
    #pragma endregion

    #pragma region "creating conditions"
    pthread_mutex_init(&cond_mutex_breakstop, nullptr);
    pthread_cond_init(&cond_var_breakstop, nullptr);
    pthread_mutex_init(&cond_mutex_continuestop, nullptr);
    pthread_cond_init(&cond_var_continuestop, nullptr);
    pthread_mutex_init(&wake_up_mutex, nullptr);
    pthread_cond_init(&wake_up_cond, nullptr);
    pthread_mutex_init(&waiting_mutex, nullptr);
    pthread_mutex_init(&stop_mutex, nullptr);
    pthread_mutex_init(&lock_mutex, nullptr);
    pthread_mutex_init(&start_mutex, nullptr);
    pthread_cond_init(&start_cond, nullptr);
    pthread_mutex_init(&cond_mutex_stop_smoking, nullptr);
    pthread_cond_init(&cond_stop_smoking, nullptr);
    pthread_mutex_init(&smoker_count_mutex, nullptr);
    #pragma endregion

    #pragma region "creating threads for proper privates"
    auto *threads = new pthread_t[priv_num];
    int temp_priv = priv_num;
    for(int i = 0; i < temp_priv; i++){
        pthread_create(&threads[i], nullptr, clean, (void*)&priv[i]);
    }
    #pragma endregion

    #pragma region "creating threads for sneaky smokers"
    pthread_t *smoker_threads;
    if(smoker_num > 0){
        smoker_threads = new pthread_t[smoker_num];
        for(int i = 0; i < smoker_num; i++){
        pthread_create(&smoker_threads[i], nullptr, smoking, (void*)&smoker[i]);
        }
    }
    #pragma endregion

    #pragma region "creating thread for commander"
    pthread_t komutanimiz_deniz_sayin;
    pthread_create(&komutanimiz_deniz_sayin, nullptr, command_func, nullptr);
    #pragma endregion

    #pragma region "waiting for smoker threads to finish"
    for(int i = 0; i < smoker_wait; i++){
        pthread_join(smoker_threads[i], nullptr);
    }
    #pragma endregion

    #pragma region "waiting for threads to finish"
    for(int i = 0; i < wait_count; i++){
        pthread_join(threads[i], nullptr);
    }
    #pragma endregion



    //wait for the komutanimiz
    pthread_join(komutanimiz_deniz_sayin, nullptr);

    return 0;
}

