#include "signal.h"
#include <stdio.h>
#include <stdint.h>
#include <rtcdc.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include <pthread.h>

#define BUFFER_SIZE 1024

typedef struct rtcdc_peer_connection rtcdc_peer_connection;
typedef struct rtcdc_data_channel rtcdc_data_channel;
typedef struct peer_info peer_info;

typedef struct sctp_packet{
    int index;
    char data[BUFFER_SIZE];
    int data_len;
} sctp_packet;

struct rtcdc_data_channel *offerer_dc;
static uv_loop_t *main_loop;
static uv_buf_t iov; 
static uv_fs_t open_req,read_req; 

char *local_file_path, *remote_file_path;

void on_open_file();


void on_open_channel(){
    fprintf(stderr, "open channel!\n");
    uv_fs_open(main_loop, &open_req, local_file_path, O_RDONLY, 0, on_open_file);
}

void on_message(rtcdc_data_channel* dc, int datatype, void* data, size_t len, void* user_data){
    sctp_packet* packet_instance = (sctp_packet*)data;

    char *msg = (char*)calloc(1, (packet_instance->data_len)*sizeof(char));
    strncpy(msg, (char*)packet_instance->data, packet_instance->data_len); 

    int index = packet_instance->index;
    printf("index:%d\n", index);
}

void on_close_channel(){
    fprintf(stderr, "close channel!\n");
}

void on_channel(rtcdc_peer_connection *peer, rtcdc_data_channel* dc, void* user_data){
    dc->on_message = on_message;
    fprintf(stderr, "on channel!!\n");
}

void on_candidate(rtcdc_peer_connection *peer, char *candidate, void *user_data ){
    peer_info* peer_sample = (peer_info*) user_data;

    //=====================start signaling====================================

    FILE *f_local_candidate;
    char *abs_file_path = signal_get_SDP_filename(peer_sample->local_peer, "/candidates");
    f_local_candidate = fopen(abs_file_path, "a");
    if(f_local_candidate == NULL){
        fprintf(stderr, "Error while opening the file.\n");
        exit(1);
    }
    fprintf(f_local_candidate, "%s\r\n", candidate);
    free(abs_file_path);
    fclose(f_local_candidate);  
    //=====================finish signaling====================================
    //printf("%s\n", candidate);
}


void on_connect(){
    printf("on connect!\n");
}


void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    *buf = uv_buf_init((char*) malloc(suggested_size), suggested_size);
}


void on_close_file(uv_fs_t* req){
    printf("close file\n");
}


void on_read_file(uv_fs_t *req) {
    if (req->result < 0) {
        fprintf(stderr, "Read error: %s\n", uv_strerror(req->result));
    }else if (req->result == 0) {
        uv_fs_t close_req;
        // synchronous
        uv_fs_close(uv_default_loop(), &close_req, open_req.result, on_close_file);
    }else if (req->result > 0) {

        iov.len = req->result;
        printf("send msg\n"); 
        rtcdc_send_message(offerer_dc, RTCDC_DATATYPE_STRING, (void *)iov.base, iov.len);
        usleep(3000);
        /*
        static int packet_index = 0;
        sctp_packet* packet_instance = (sctp_packet*)calloc(1, sizeof(sctp_packet));

        if(packet_index == 0 && iov.len == BUFFER_SIZE)
        {
            //send the first packet which contains destination file path
            packet_instance->index = packet_index;
            strcpy(packet_instance->data, remote_file_path);
            packet_instance->data_len = 11;
            rtcdc_send_message(offerer_dc, RTCDC_DATATYPE_STRING, (void *)packet_instance, sizeof(sctp_packet));
            packet_index++;

            packet_instance->index = packet_index;
            strcpy(packet_instance->data,iov.base);
            packet_instance->data_len = iov.len;
            rtcdc_send_message(offerer_dc, RTCDC_DATATYPE_STRING, (void *)packet_instance, sizeof(sctp_packet));
            packet_index++;
        }else if(packet_index == 0 && iov.len < BUFFER_SIZE){
            //if the file is so small that need to send only one time
            //send the first packet which contains destination file path
            packet_instance->index = packet_index;
            strcpy(packet_instance->data, remote_file_path);
            packet_instance->data_len = 11;      
            rtcdc_send_message(offerer_dc, RTCDC_DATATYPE_STRING, (void *)packet_instance, sizeof(sctp_packet));
            packet_index++;                      

            packet_instance->index = -1;
            strcpy(packet_instance->data,iov.base);
            packet_instance->data_len = iov.len; 
            rtcdc_send_message(offerer_dc, RTCDC_DATATYPE_STRING, (void *)packet_instance, sizeof(sctp_packet));

            printf("Finish sending\n");

        }else if (packet_index > 0 && iov.len == BUFFER_SIZE){
            packet_instance->index = packet_index;
            strcpy(packet_instance->data,iov.base);
            packet_instance->data_len = iov.len;
            rtcdc_send_message(offerer_dc, RTCDC_DATATYPE_STRING, (void *)packet_instance, sizeof(sctp_packet));
            usleep(800000);
            packet_index++;
        }else if (packet_index > 0 && iov.len < BUFFER_SIZE){
            //send the last packet
            packet_instance->index = -1;
            strcpy(packet_instance->data,iov.base);
            packet_instance->data_len = iov.len;
            rtcdc_send_message(offerer_dc, RTCDC_DATATYPE_STRING, (void *)packet_instance, sizeof(sctp_packet));

            printf("Finish sending\n");
        }
*/

        memset(iov.base, '\0', iov.len);
        uv_fs_read(main_loop, req, open_req.result, &iov, 1, -1, on_read_file);

    }
}


void on_open_file(uv_fs_t *req){
    if (req->result >= 0){
        printf("open file\n");
        char* buffer = (char*)calloc(1, BUFFER_SIZE*sizeof(char));
        iov = uv_buf_init(buffer, BUFFER_SIZE);
        int res = uv_fs_read(main_loop, &read_req, req->result, &iov, 1, -1, on_read_file);
    }
    else {
        fprintf(stderr, "error opening file: %s\n", uv_strerror((int)req->result));
    }
}


void trans_file(uv_work_t *work){
    //TODO infinite retry
    sleep(5);

    rtcdc_peer_connection* offerer = (rtcdc_peer_connection*)work->data;
    offerer_dc = rtcdc_create_data_channel(offerer, "Demo Channel", "", on_open_channel, on_message, NULL, NULL); 
    if(offerer_dc != NULL){
        //uv_fs_open(main_loop, &open_req, local_file_path, O_RDONLY, 0, on_open_file);
    }
}

void sync_loop(uv_work_t *work){
    rtcdc_peer_connection* offerer = (rtcdc_peer_connection*)work->data;
    rtcdc_loop(offerer);
}


int main(int argc, char *argv[]){
    if(argc!=5){
        fprintf(stderr, "argument error!");
        return 0;
    }

    // argv[1] is local peer name
    // argv[2] is remote peer name

    peer_info* peer_sample = (peer_info*)calloc(1, sizeof(peer_info));
    peer_sample->local_peer = argv[1];
    peer_sample->remote_peer = argv[2];
    local_file_path = argv[3];
    remote_file_path = argv[4];


    // clear the content of local_candidate file
    FILE *f_local_candidate;
    char *abs_file_path = signal_get_SDP_filename(peer_sample->local_peer, "/candidates");
    f_local_candidate = fopen(abs_file_path, "w");
    if(f_local_candidate == NULL){
        fprintf(stderr, "Error while clear the file.\n");
        exit(1);
    }
    fclose(f_local_candidate);  

    // create rtcdc peer
    // the peer_sample is the local&remote infomation

    // on_candidate will be called if create peer connection successfully
    // on_candidate store the candidate in the local_candidate file
    rtcdc_peer_connection* offerer = rtcdc_create_peer_connection((void*)on_channel, (void*)on_candidate, on_connect, "stun.services.mozilla.com", NULL, (void*)peer_sample);

    // generate local SDP and store in the file
    signal_gen_local_SDP_file(offerer, peer_sample);

    printf("ready to connect?(y/n)");
    if(fgetc(stdin)=='y'){
        signal_get_remote_SDP_file(offerer, peer_sample); 

        // clear the content of local_candidate file
        f_local_candidate = fopen(abs_file_path, "w");
        if(f_local_candidate == NULL){
            fprintf(stderr, "Error while clear the file.\n");
            exit(1);
        }
        free(abs_file_path);
        fclose(f_local_candidate);  

        signal_get_remote_candidate_file(offerer, peer_sample);
        signal_gen_local_SDP_file(offerer, peer_sample);

        main_loop = uv_default_loop();         
        uv_work_t work[2];
        work[0].data = (void*)offerer;
        work[1].data = (void*)offerer;
        uv_queue_work(main_loop, &work[0], sync_loop, NULL);
        uv_queue_work(main_loop, &work[1], trans_file, NULL);
        return uv_run(main_loop, UV_RUN_DEFAULT);
    }

    return 0;
}
