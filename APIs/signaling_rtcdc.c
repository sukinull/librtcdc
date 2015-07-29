#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "signaling_rtcdc.h"

void upload_client_on_message()
{}


void upload_fs_on_message()
{}


void on_open_channel()
{}


void on_candidate(struct rtcdc_peer_connection *peer, 
                    char *candidate, 
                    void *user_data)
{
#ifdef DEBUG_RTCDC
    fprintf(stderr, "signaling_rtcdc: on candidate\n");
#endif
    char *endl = "\n";
    char *candidate_set = ((struct sy_rtcdc_info_t *)user_data)->local_candidates;
    strcat(candidate_set, candidate);
    strcat(candidate_set, endl);
}


void upload_fs_on_channel(struct rtcdc_peer_connection *peer,
                struct rtcdc_data_channel *dc,
                void *user_data)
{
    dc->on_message = upload_fs_on_message;
#ifdef DEBUG_RTCDC
    fprintf(stderr, "signaling_rtcdc: upload fs on_channel\n");
#endif
}


void upload_client_on_channel(struct rtcdc_peer_connection *peer,
                struct rtcdc_data_channel *dc,
                void *user_data)
{
    dc->on_message = upload_client_on_message;
#ifdef DEBUG_RTCDC
    fprintf(stderr, "signaling_rtcdc: upload client on_channel\n");
#endif
}


void upload_fs_on_connect(struct rtcdc_peer_connection *peer
                        ,void *user_data)
{
#ifdef DEBUG_RTCDC
    fprintf(stderr, "signaling_rtcdc: upload fs on_connect\n");
    fprintf(stderr, "signaling_rtcdc: start create data channel\n");
#endif
    struct rtcdc_data_channel *upload_dc = rtcdc_create_data_channel(peer, "Upload Channel",
                                                                    "", on_open_channel,
                                                                    NULL, NULL, NULL);
    if(upload_dc == NULL)
        fprintf(stderr, "signaling_rtcdc: fail create data channel\n");
}


void upload_client_on_connect(struct rtcdc_peer_connection *peer
                        ,void *user_data)
{
#ifdef DEBUG_RTCDC
    fprintf(stderr, "signaling_rtcdc: upload client on_connect\n");
    fprintf(stderr, "signaling_rtcdc: start create data channel\n");
#endif
    struct rtcdc_data_channel *upload_dc = rtcdc_create_data_channel(peer, "Upload Channel",
                                                                    "", on_open_channel,
                                                                    NULL, NULL, NULL);
    if(upload_dc == NULL)
        fprintf(stderr, "signaling_rtcdc: fail create data channel\n");
}


void uv_rtcdc_loop(uv_work_t *work)
{
    struct rtcdc_peer_connection *peer = (struct rtcdc_peer_connection *)work->data;
    rtcdc_loop(peer);
}

char *signal_getline(char **string)
{
    char *line = (char *)calloc(1, 128*sizeof(char));
    int str_iter;
    if((*string)[0] == 0)
        return NULL;
    for (str_iter = 0; ; str_iter++)
    {
        if((*string)[str_iter] != '\n')
            line[str_iter] = (*string)[str_iter];
        else
        {    
            *string+=(str_iter+1); 
            break;
        }
    }
    return line;

}
void parse_candidates(struct rtcdc_peer_connection *peer, char *candidates)
{
    char *line;
    while((line = signal_getline(&candidates)) != NULL){    
        rtcdc_parse_candidate_sdp(peer, line);
        // printf("%s\n", line);
    }
}
