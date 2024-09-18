#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100
#define ACK 1
#define REQ 2
#define SEG 3
#define EOM 4
#define UPD 5
#define FIN 6
#define END 7

typedef struct File {
    int id;
    int n_segments;
    char (*segments)[HASH_SIZE + 1];
} File;

typedef struct Peer_args {
    int rank;
    int n_files;
    int numtasks;
} Peer_args;

File* owned_files;
File* wanted_files;

// receives list from the tracker with all peers/seeds from which
// the client can request a segment; peer_list contains the hashes
// of that every client has from the requested file
File* getPeerList(int numtasks, File current_file) {

    // allocating memory for the list
    File *peer_list = malloc(sizeof(File) * numtasks);
    for (int i = 0; i < numtasks; i++) {
        peer_list[i].n_segments = current_file.n_segments;
        peer_list[i].segments = malloc(current_file.n_segments * sizeof(*(peer_list[i].segments)));
        for (int j = 0; j < current_file.n_segments; j++)
            strcpy(peer_list[i].segments[j], "");
        peer_list[i].id = current_file.id;
    }

    // receive list of segments for every client
    while (1) {
        int signal = -1;
        MPI_Recv(&signal, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
       
        // we can either get a segment or a signal that the transmission of peers ended
        if (signal == EOM) {
            break;
        }
        
        // receive the current segment and store it into the list
        int segment_id = -1;
        int peer = -1;
        MPI_Recv(&segment_id, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Recv(&peer, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Recv(peer_list[peer].segments[segment_id], HASH_SIZE + 1, MPI_CHAR, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }
    return peer_list;
}

void *download_thread_func(void *arg)
{
    Peer_args args = *(Peer_args*) arg;
    int rank = args.rank;
    int n_files = args.n_files;
    int numtasks = args.numtasks;

    // array used to alternate between download sources
    // counts for every file how many times the client has been used
    int **times_used = malloc(sizeof(int*) * (MAX_FILES + 1));
    for (int i = 0; i < MAX_FILES + 1; i++) {
        times_used[i] = malloc(sizeof(int) * numtasks);
        for (int j = 0; j < numtasks; j++)
            times_used[i][j] = 0;
    }

    for (int i = 0; i < n_files; i++) {
        // requesting seed/peer list from tracker
        int signal = REQ;
        MPI_Send(&signal, 1, MPI_INT, 0, 1, MPI_COMM_WORLD);

        // sending wanted file id
        MPI_Send(&wanted_files[i].id, 1, MPI_INT, 0, 0, MPI_COMM_WORLD);
        File current_file;
        current_file.id = wanted_files[i].id;

        // receiving number of segments and list of peers from the tracker
        MPI_Recv(&current_file.n_segments, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        owned_files[current_file.id].id = current_file.id;
        owned_files[current_file.id].n_segments = current_file.n_segments;
        File* peer_list = getPeerList(numtasks, current_file);

        int cnt = 0;

        // starting to download every segment of the file
        for (int k = 0; k < current_file.n_segments; k++) {

            // every 10 downloads update the tracker with the new segments that I own
            // and ask for an updated peer list
            if (cnt == 10) {
                // send an update message
                int signal = UPD;
                MPI_Send(&signal, 1, MPI_INT, 0, 1, MPI_COMM_WORLD);

                // send the hashes of the files that I own from the current file
                for (int j = 0; j < current_file.n_segments; j++) {
                
                    if (strlen(owned_files[current_file.id].segments[j]) > 0) {
                        signal = SEG;
                        MPI_Send(&signal, 1, MPI_INT, 0, 1, MPI_COMM_WORLD);
                        MPI_Send(&j, 1, MPI_INT, 0, 0, MPI_COMM_WORLD);
                        MPI_Send(&current_file.id, 1, MPI_INT, 0, 0, MPI_COMM_WORLD);
                        MPI_Send(owned_files[current_file.id].segments[j], HASH_SIZE + 1, MPI_CHAR, 0, 0, MPI_COMM_WORLD);
                    }
                }

                // signal that the transmission of segments ended
                signal = EOM;
                MPI_Send(&signal, 1, MPI_INT, 0, 1, MPI_COMM_WORLD);

                // request a new peer list
                cnt = 0;
                for (int i = 0; i < numtasks; i++) {
                    free(peer_list[i].segments);
                }
                free(peer_list);
                signal = REQ;
                MPI_Send(&signal, 1, MPI_INT, 0, 1, MPI_COMM_WORLD);
                MPI_Send(&current_file.id, 1, MPI_INT, 0, 0, MPI_COMM_WORLD);
                MPI_Recv(&current_file.n_segments, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                peer_list = getPeerList(numtasks, current_file);

            }
            
            // choose a source to download the current segment from based on how many times
            // each client has been requested for this file and picking the least used one
            int chosen_peer = -1;
            if (strlen(owned_files[current_file.id].segments[k]) == 0) {
                for (int p = 1; p < numtasks; p++) {
                    if (p != rank) {
                        if (strlen(peer_list[p].segments[k]) > 0 && (chosen_peer == -1 || times_used[current_file.id][p] < times_used[current_file.id][chosen_peer])) {
                            chosen_peer = p;
                        }
                    }
                }
            }

            // increment the count for the chosen peer
            times_used[current_file.id][chosen_peer]++;

            // simulate downloading the segment using an ACK signal
            int signal = REQ;
            do {
                // requesting the current segment from the chosen peer
                signal = REQ;
                MPI_Send(&signal, 1, MPI_INT, chosen_peer, 1, MPI_COMM_WORLD);
                MPI_Send(peer_list[chosen_peer].segments[k], HASH_SIZE + 1, MPI_CHAR, chosen_peer, 1, MPI_COMM_WORLD);

                // receiving the segment (simulated with an ACK signal)
                MPI_Recv(&signal, 1, MPI_INT, chosen_peer, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            } while (signal != ACK);

            // copy the segment in our memory
            strcpy(owned_files[current_file.id].segments[k], peer_list[chosen_peer].segments[k]);
            cnt++;
        }

        int id = wanted_files[i].id;

        // sending a signal to the tracker that we finished downloading a file
        // to be added to the seeds of that file
        signal = FIN;
        MPI_Send(&signal, 1, MPI_INT, 0, 1, MPI_COMM_WORLD);
        MPI_Send(&id, 1, MPI_INT, 0, 0, MPI_COMM_WORLD);

        // writing the downloaded file on the disk
        char output_file[MAX_FILENAME];
        sprintf(output_file, "client%d_file%d", rank, id);
        FILE* new_file = fopen(output_file, "w");

        for (int k = 0; k < owned_files[id].n_segments; k++) {
            fprintf(new_file, "%s\n", owned_files[id].segments[k]);
        }

        fclose(new_file);
        for (int i = 0; i < numtasks; i++) {
            free(peer_list[i].segments);
        }
        free(peer_list);
    }

    // sending a signal to the tracker that we completed all of our downloads
    int signal = END;
    MPI_Send(&signal, 1, MPI_INT, 0, 1, MPI_COMM_WORLD);

    // freeing the allocated memory for download
    for (int i = 0; i < MAX_FILES + 1; i++)
        free(times_used[i]);
    free(times_used);
    return NULL;
}

void *upload_thread_func(void *arg)
{   
    // keep waiting for requests from other clients
    while (1) {
        int signal = -1;
        MPI_Status status;
        MPI_Recv(&signal, 1, MPI_INT, MPI_ANY_SOURCE, 1, MPI_COMM_WORLD, &status);
        int sender_rank = status.MPI_SOURCE;
        
        // if we have a segment request get the hash of the desired segment
        // and send it to the client (simulated with ACK);
        // if we get an END signal that means all clients finished downloading
        // so we can close the upload function
        if (signal == REQ) {
            char segment[HASH_SIZE + 1];
            MPI_Recv(segment, HASH_SIZE + 1, MPI_CHAR, sender_rank, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            signal = ACK;
            MPI_Send(&signal, 1, MPI_INT, sender_rank, 0, MPI_COMM_WORLD);
        } else if (signal == END) {
            break;
        }
    }

    return NULL;
}

void tracker(int numtasks, int rank) {
    // structures to keep track of all files from every client
    // and the peers/seeds for every file
    File** all_files;
    int** swarms;
    int** seeds;

    int n_clients = numtasks - 1;
    swarms = malloc(sizeof(int*) * (MAX_FILES + 1));
    for (int i = 0; i < MAX_FILES + 1; i++) {
        swarms[i] = malloc(sizeof(int) * numtasks);
        for (int j = 0; j < numtasks; j++)
            swarms[i][j] = 0;
    }

    seeds = malloc(sizeof(int*) * (MAX_FILES + 1));
    for (int i = 0; i < MAX_FILES + 1; i++) {
        seeds[i] = malloc(sizeof(int) * numtasks);
        for (int j = 0; j < numtasks; j++)
            seeds[i][j] = 0;
    }


    all_files = malloc(sizeof(File*) * numtasks);
    
    for (int i = 0; i < numtasks; i++) {
        all_files[i] = malloc(sizeof(File) * (MAX_FILES + 1));
        for (int j = 0; j < MAX_FILES + 1; j++) {
            all_files[i][j].n_segments = 0;
            all_files[i][j].segments = malloc(MAX_CHUNKS * sizeof(*(all_files[i][j].segments)));
            for (int k = 0; k < MAX_CHUNKS; k++)
                strcpy(all_files[i][j].segments[k], "");
        }
    }

    // receiving the files that every client owns in the beginning
    for (int i = 0; i < numtasks - 1; i++) {
        MPI_Status status;
        int sender_rank = 0;
        int n_files = 0;

        // receive the number of files that the client wants to send
        MPI_Recv(&n_files, 1, MPI_INT, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &status);
        sender_rank = status.MPI_SOURCE;

        for (int j = 0; j < n_files; j++) {
            File file;

            // receive the file id and number of segments and add the sender to the swarm and seed of that file
            MPI_Recv(&file.id, 1, MPI_INT, sender_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            swarms[file.id][sender_rank] = 1;
            seeds[file.id][sender_rank] = 1;
            MPI_Recv(&all_files[sender_rank][file.id].n_segments, 1, MPI_INT, sender_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            for (int k = 1; k < numtasks; k++)
                all_files[k][file.id].n_segments = all_files[sender_rank][file.id].n_segments;
            all_files[sender_rank][file.id].id = file.id;

            // receive all the segment hashes of the file
            for (int k = 0; k < all_files[sender_rank][file.id].n_segments; k++) {
                MPI_Recv(all_files[sender_rank][file.id].segments[k], HASH_SIZE + 1, MPI_CHAR, sender_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            }
        }
    }

    // sending confirmation to every client that they can start downloading
    int signal = ACK;

    for (int i = 1; i < numtasks; i++) {
        MPI_Send(&signal, 1, MPI_INT, i, 0, MPI_COMM_WORLD);
    }

    // starting to respond to messages while there are still clients that want to download
    while (n_clients > 0) {
        int signal = -1;
        MPI_Status status;
        MPI_Recv(&signal, 1, MPI_INT, MPI_ANY_SOURCE, 1, MPI_COMM_WORLD, &status);

        int sender_rank = status.MPI_SOURCE;
        int file_id = -1;
        switch(signal) {
            // in case of a request the tracker will send a list of all the clients
            // that have segments of the requested file, and their hashes
            case REQ:

                // receive the requested file id
                MPI_Recv(&file_id, 1, MPI_INT, sender_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                // send the number of segments that the client has to download
                for (int i = 1; i < numtasks; i++)
                    if (swarms[file_id][i] == 1) {
                        MPI_Send(&all_files[i][file_id].n_segments, 1, MPI_INT, sender_rank, 0, MPI_COMM_WORLD);
                        break;
                    }
                
                // start sending the peer list
                for (int i = 1; i < numtasks; i++) {
                    if (swarms[file_id][i] == 1 || seeds[file_id][i] && i != sender_rank) {
                        // client i owns segments of the current file
                        for (int j = 0; j < all_files[i][file_id].n_segments; j++) {
                            // if client i has that segment send it
                            if (strlen(all_files[i][file_id].segments[j]) > 0) {
                                int signal = SEG;
                                MPI_Send(&signal, 1, MPI_INT, sender_rank, 0, MPI_COMM_WORLD);
                                MPI_Send(&j, 1, MPI_INT, sender_rank, 0, MPI_COMM_WORLD);
                                MPI_Send(&i, 1, MPI_INT, sender_rank, 0, MPI_COMM_WORLD);
                                MPI_Send(all_files[i][file_id].segments[j], HASH_SIZE + 1, MPI_CHAR, sender_rank, 0, MPI_COMM_WORLD);
                            }
                        }
                    }
                }

                // signal that the list as been completely sent
                signal = EOM;
                MPI_Send(&signal, 1, MPI_INT, sender_rank, 0, MPI_COMM_WORLD);
                break;
            // in case of an update the trackers needs to update its local database
            case UPD:
                while (1) {
                    int signal = -1;
                    MPI_Recv(&signal, 1, MPI_INT, sender_rank, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                    // we can either get a segment or a signal that the transmission of peers ended
                    if (signal == EOM) {
                        break;
                    }

                    // receive hashes of the segments that the client has and add them to the tracker's
                    // array of files; also add the sender to the swarm of that file
                    int segment_id = -1;
                    int file_id = -1;
                    MPI_Recv(&segment_id, 1, MPI_INT, sender_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                    MPI_Recv(&file_id, 1, MPI_INT, sender_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                    MPI_Recv(all_files[sender_rank][file_id].segments[segment_id], HASH_SIZE + 1, MPI_CHAR, sender_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                    swarms[file_id][sender_rank] = 1;
                }

                break;
            // in case of a FIN a client completed a download so we add that client to the file's seeds
            case FIN:
                // receive the file id
                file_id = -1;
                MPI_Recv(&file_id, 1, MPI_INT, sender_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                // making sure the tracker knows that the client has all the segments of the file
                // in case an update with the last segments didn't happen

                for (int i = 1; i < numtasks; i++) {
                    if (seeds[file_id][i]) {
                        for (int j = 0; j < all_files[i][file_id].n_segments; j++) {
                            strcpy(all_files[sender_rank][file_id].segments[j], all_files[i][file_id].segments[j]);
                        }
                    }
                }

                seeds[file_id][sender_rank] = 1;
                break;
            // a client completed all downloads, so decrement the total number of clients
            case END:
                n_clients--;
                break;
            default:
                break;
        }
    }

    // if everyone finished downloading send a signal to all clients
    // to close their upload function
    signal = END;
    for (int i = 1; i < numtasks; i++) {
        MPI_Send(&signal, 1, MPI_INT, i, 1, MPI_COMM_WORLD);
    }

    // freeing the allocated memory for the tracker
    for (int i = 0; i < MAX_FILES + 1; i++)
        free(swarms[i]);
    free(swarms);

    for (int i = 0; i < MAX_FILES + 1; i++)
        free(seeds[i]);
    free(seeds);

    for (int i = 0; i < numtasks; i++) {
        for (int j = 0; j < MAX_FILES + 1; j++) {
            free(all_files[i][j].segments);
        }
        free(all_files[i]);
    }

    free(all_files);
}   

void peer(int numtasks, int rank) {
    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;
    int r;
    char input_file[MAX_FILENAME];
    int n_owned_files = 0;
    int n_wanted_files = 0;

    sprintf(input_file, "in%d.txt", rank);
    FILE *fp = fopen(input_file, "r");

    fscanf(fp, "%d", &n_owned_files);
    owned_files = malloc(sizeof(File) * (MAX_FILES + 1));

    fgetc(fp);

    for (int i = 1; i <= MAX_FILES; i++) {
        owned_files[i].n_segments = 0;
        owned_files[i].id = 0;
        owned_files[i].segments = malloc(MAX_CHUNKS * sizeof(*(owned_files[i].segments)));
        for (int k = 0; k < MAX_CHUNKS; k++)
                strcpy(owned_files[i].segments[k], "");
    }

    // reading the files that the current client owns
    for (int i = 0; i < n_owned_files; i++) {
        char line[100];
        char filename[MAX_FILENAME];
        fgets(line, sizeof(line), fp);
        int n = 0;

        sscanf(line, "%s %d", filename, &n);
        int id = filename[strlen(filename) - 1] - '0';

        owned_files[id].id = id;
        owned_files[id].n_segments = n;

        for (int j = 0; j < owned_files[id].n_segments; j++) {
            fgets(line, sizeof(line), fp);
            sscanf(line, "%s", owned_files[id].segments[j]);
        }

    }

    // sending all the files to the tracker, first send the number of files
    MPI_Send(&n_owned_files, 1, MPI_INT, 0, 0, MPI_COMM_WORLD);
    
    // if we own the current file send all its segments to the tracker
    for (int i = 1; i <= MAX_FILES; i++) {
        if (owned_files[i].n_segments == 0) {
            continue;
        }
        MPI_Send(&i, 1, MPI_INT, 0, 0, MPI_COMM_WORLD);
        MPI_Send(&owned_files[i].n_segments, 1, MPI_INT, 0, 0, MPI_COMM_WORLD);

        for (int j = 0; j < owned_files[i].n_segments; j++)
            MPI_Send(owned_files[i].segments[j], HASH_SIZE + 1, MPI_CHAR, 0, 0, MPI_COMM_WORLD);
    }
    
    // reading the files that the current client wants to download
    fscanf(fp, "%d", &n_wanted_files);
    fgetc(fp);

    wanted_files = malloc(sizeof(File) * n_wanted_files);
    for (int i = 0; i < n_wanted_files; i++) {
        char line[100];
        char filename[MAX_FILENAME];

        fgets(line, sizeof(line), fp);
        sscanf(line, "%s", filename);
        wanted_files[i].id = filename[strlen(filename) - 1] - '0';
    }

    fclose(fp);

    // waiting for confirmation from tracker that it received all the information
    int signal;
    do {
        signal = -1;
        MPI_Recv(&signal, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    } while (signal != ACK);

    // client can now begin downloading, starting download and upload threads
    Peer_args args;
    args.rank = rank;
    args.n_files = n_wanted_files;
    args.numtasks = numtasks;
    r = pthread_create(&download_thread, NULL, download_thread_func, (void *) &args);

    if (r) {
        printf("Eroare la crearea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de upload\n");
        exit(-1);
    }

    r = pthread_join(download_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_join(upload_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de upload\n");
        exit(-1);
    }

    // freeing allocated memory for the peers
    for (int i = 1; i <= MAX_FILES; i++) {
        free(owned_files[i].segments);
    }

    free(owned_files);
    free(wanted_files);
}
 
int main (int argc, char *argv[]) {
    int numtasks, rank;
 
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
        exit(-1);
    }
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();

}
