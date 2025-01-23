#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100

#define UPLOAD_TAG 100
#define FILE_TAG 101
#define TRACKER_REQUEST_TAG 102
#define TRACKER_ANSWER_TAG 103
#define CHUNK_TAG 104


// Structuri noi
typedef struct {
    char hash[HASH_SIZE];
    int owners[100];
    int nr_owners;
    int index;
} chunk_info;

int numtasks;

typedef struct {
    char filename[MAX_FILENAME];
    chunk_info chunks[MAX_CHUNKS];
    int nr_chunks;
    bool is_owned;
    int chunks_recv[MAX_CHUNKS];
    int nr_chunks_recv;
} file_entry;

file_entry files_owned[MAX_FILES];
file_entry requested_files[MAX_FILES];

typedef struct {
    file_entry files[MAX_FILES];
    int files_count;
} tracker_db;

tracker_db tracker;

typedef struct {
    char filename[MAX_FILENAME];
    int chunk_index;
    char hash[HASH_SIZE];
} chunk_request;

typedef struct {
    char request[50];
    char filename[MAX_FILENAME];
} tracker_request;

typedef struct {
    char filename[MAX_FILENAME];
    int chunks;
} file_info_request;

int nr_req_files = 0;
int completed_files = 0;

int files_owned_count = 0;


// function to create client file
void make_file_client(file_entry *file, int chunk_index, int rank) {
    // Creare nume fișier client
    char clientfilename[MAX_FILENAME + 10];
    snprintf(clientfilename, sizeof(clientfilename), "client%d_%s", rank, file->filename);

    FILE *output;
    if (chunk_index == 0) {
        output = fopen(clientfilename, "w");
    } else {
        output = fopen(clientfilename, "a");
    }

    fwrite(file->chunks[chunk_index].hash, sizeof(char), HASH_SIZE, output);
    fwrite("\n", sizeof(char), 1, output);
    fclose(output);
}

// function to request chunks information from tracker
void request_file_info_from_tracker_db(file_entry *file, int rank) {
    tracker_request req;
    strcpy(req.request, "REQ_O");
    strcpy(req.filename, file->filename);

    MPI_Send(&req, sizeof(tracker_request), MPI_BYTE, TRACKER_RANK, TRACKER_REQUEST_TAG, MPI_COMM_WORLD);
    
    MPI_Recv(&file->nr_chunks, 1, MPI_INT, TRACKER_RANK, TRACKER_ANSWER_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    for (int i = 0; i < file->nr_chunks; i++) {
        MPI_Recv(&file->chunks[i].nr_owners, 1, MPI_INT, TRACKER_RANK, TRACKER_ANSWER_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Recv(&file->chunks[i].index, 1, MPI_INT, TRACKER_RANK, TRACKER_ANSWER_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Recv(file->chunks[i].owners, file->chunks[i].nr_owners, MPI_INT, TRACKER_RANK, TRACKER_ANSWER_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }
}

// function to request chunk from peer
void peer_chunk_request(file_entry *file, int file_index, int chunk_index, int rank) {
    srand(time(NULL));  // Inițializează generatorul de numere aleatoare
    int chosen_owner = file->chunks[chunk_index].owners[rand() % file->chunks[chunk_index].nr_owners]; // Alegere deținător fragment random de la 0 la owners count
    printf("Chunk %d is from owner: %d\n", file->chunks[chunk_index].index, chosen_owner);

    // Creare cerere pentru fragment
    chunk_request req;
    strcpy(req.filename, file->filename);
    req.chunk_index = chunk_index;

    // Trimite cerere
    MPI_Send(&req, sizeof(chunk_request), MPI_BYTE, chosen_owner, UPLOAD_TAG, MPI_COMM_WORLD);

    // Primește hash-ul fragmentului
    MPI_Recv(req.hash, HASH_SIZE + 1, MPI_CHAR, chosen_owner, CHUNK_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    // Actualizare fișier cerut cu fragmentul primit
    strcpy(file->chunks[chunk_index].hash, req.hash);
    file->chunks_recv[chunk_index] = chosen_owner;
    file->nr_chunks_recv++;
}
// function to try and complete file
void manage_requested_files(file_entry *file, int rank) {

    // Cerere de informații despre fragmente de la tracker
    request_file_info_from_tracker_db(file, rank);

    // Cerere pentru fiecare fragment în parte
    for (int i = 0; i < file->nr_chunks; i++) {
        if (file->chunks_recv[i] == -1) {
            peer_chunk_request(file, file - requested_files, i, rank);
        }
    }

    // Verificare dacă fișierul a fost complet descărcat
    if (file->nr_chunks_recv == file->nr_chunks) {

        // Crearea fișierului client
        for (int i = 0; i < file->nr_chunks; i++) {
            make_file_client(file, i, rank);
        }
        completed_files++;
    }
}

// function to be executed by download thread
void *download_thread_func(void *arg) {
    int rank = *(int *)arg;

    while (completed_files < nr_req_files) {
        for (int i = 0; i < nr_req_files; i++) {
            manage_requested_files(&requested_files[i], rank);
        }
    }

    // Trimite mesaj READY către tracker după completarea tuturor fișierelor
    char all_good[50] = "RDY";
    MPI_Send(all_good, 50, MPI_CHAR, TRACKER_RANK, TRACKER_REQUEST_TAG, MPI_COMM_WORLD);

    return NULL;
}



// function to be executed by upload thread
void *upload_thread_func(void *arg) {
    int rank = *(int *)arg;

    while (1) {
        MPI_Status status;
        chunk_request req;

        // Primește cererea de fragment de la peer
        MPI_Recv(&req, sizeof(chunk_request), MPI_BYTE, MPI_ANY_SOURCE, UPLOAD_TAG, MPI_COMM_WORLD, &status);

        // Dacă cererea este pentru terminare, închide thread-ul
        if (strcmp(req.filename, "GATA") == 0) {
            printf("GATA from peer %d\n", rank);
            return NULL;
        }

        // Găsește fișierul deținut și trimite fragmentul cerut
        for (int i = 0; i < files_owned_count; i++) {
            if (strcmp(files_owned[i].filename, req.filename) == 0) {
                MPI_Send(files_owned[i].chunks[req.chunk_index].hash,
                         HASH_SIZE + 1, MPI_CHAR,
                         status.MPI_SOURCE, CHUNK_TAG, MPI_COMM_WORLD);
                break;
            }
        }
    }
}

// function to receive files information from peers and add them to tracker database
void tracker_database_add(int numtasks, int rank) {
    int owned_files;
    file_info_request file_info[MAX_FILES];
    char hash[HASH_SIZE];

    for (int i = 1; i < numtasks; i++) {
        // Primește numărul total de fișiere
        MPI_Recv(&owned_files, 1, MPI_INT, i, FILE_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        // Primește informațiile despre fișiere
        MPI_Recv(file_info, sizeof(file_info_request) * owned_files, MPI_BYTE, i, FILE_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        for (int j = 0; j < owned_files; j++) {
            file_entry *file = &tracker.files[tracker.files_count];
            strcpy(file->filename, file_info[j].filename);
            file->nr_chunks = file_info[j].chunks;
            file->is_owned = true;

            for (int k = 0; k < file_info[j].chunks; k++) {
                MPI_Recv(hash, HASH_SIZE, MPI_CHAR, i, FILE_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                strcpy(file->chunks[k].hash, hash);
                file->chunks[k].owners[0] = i;
                file->chunks[k].nr_owners = 1;
                file->chunks[k].index = k;
            }
            tracker.files_count++;
        }
    }

    for (int i = 1; i < numtasks; i++) {
        char answer[15] = "ACK";
        MPI_Send(answer, 15, MPI_CHAR, i, FILE_TAG, MPI_COMM_WORLD);
    }
}

// function to send DONE message to all peers
void send_done_message(int numtasks) {
    char answer[10] = "GATA";

    // Trimite mesajul DONE către fiecare peer (exceptând trackerul)
    for (int i = 1; i < numtasks; i++) {
        MPI_Send(answer, 10, MPI_CHAR, i, UPLOAD_TAG, MPI_COMM_WORLD);
    }

    printf("Sent GATA to peers\n");
}

int all_peers_rdy(int *clients_rdy, int numtasks) {
    for (int i = 1; i < numtasks; i++) {
        if (clients_rdy[i] == 0) {
            return 0;
        }
    }
    return 1;
}

void tracker_func(int numtasks, int rank) {
    // Adaugă fișierele primite de la peers în baza de date a trackerului
    tracker_database_add(numtasks, rank);

    // Inițializează array-ul pentru a ține evidența clienților gata
    int clients_rdy[numtasks];
    for (int i = 0; i < numtasks; i++) {
        clients_rdy[i] = 0;
    }

    while (1) {
        tracker_request req;
        MPI_Status status;

        // Așteaptă cereri de la peers
        MPI_Recv(&req, sizeof(tracker_request), MPI_BYTE, MPI_ANY_SOURCE, TRACKER_REQUEST_TAG, MPI_COMM_WORLD, &status);


        // Cerere pentru lista proprietarilor de fragmente
        if (strcmp(req.request, "REQ_O") == 0) {

            for (int i = 0; i < tracker.files_count; i++) {
                if (strcmp(tracker.files[i].filename, req.filename) == 0) {
                    MPI_Send(&tracker.files[i].nr_chunks, 1, MPI_INT, status.MPI_SOURCE, TRACKER_ANSWER_TAG, MPI_COMM_WORLD);
                    for (int j = 0; j < tracker.files[i].nr_chunks; j++) {
                        MPI_Send(&tracker.files[i].chunks[j].nr_owners, 1, MPI_INT, status.MPI_SOURCE, TRACKER_ANSWER_TAG, MPI_COMM_WORLD);
                        MPI_Send(&tracker.files[i].chunks[j].index, 1, MPI_INT, status.MPI_SOURCE, TRACKER_ANSWER_TAG, MPI_COMM_WORLD);
                        MPI_Send(tracker.files[i].chunks[j].owners, tracker.files[i].chunks[j].nr_owners, MPI_INT, status.MPI_SOURCE, TRACKER_ANSWER_TAG, MPI_COMM_WORLD);
                    }
                    break;
                }
            }
        }

        // Peer-ul trimite mesaj READY (fișierele sunt complet descărcate)
        if (strcmp(req.request, "RDY") == 0) {
            printf("Peer %d is all good\n", status.MPI_SOURCE);
            clients_rdy[status.MPI_SOURCE] = 1;

            // Verifică dacă toți peers sunt gata
            if (all_peers_rdy(clients_rdy, numtasks)) {
                send_done_message(numtasks);
                return;
            }
        }
    }
}


void process_owned_files(int rank) {
    char infilename[MAX_FILENAME];
    snprintf(infilename, sizeof(infilename), "in%d.txt", rank);

    FILE *input = fopen(infilename, "r");
    if (input == NULL) {
        printf("Eroare la %d\n", rank);
        exit(-1);
    }

    file_info_request file_info[MAX_FILES];
    char hash[HASH_SIZE];

    fscanf(input, "%d", &files_owned_count);

    for (int i = 0; i < files_owned_count; i++) {
        fscanf(input, "%s", file_info[i].filename);
        fscanf(input, "%d", &file_info[i].chunks);

        strcpy(files_owned[i].filename, file_info[i].filename);
        files_owned[i].nr_chunks = file_info[i].chunks;
        files_owned[i].is_owned = true;

        for (int j = 0; j < file_info[i].chunks; j++) {
            fscanf(input, "%s", hash);
            hash[HASH_SIZE] = '\0';
            strcpy(files_owned[i].chunks[j].hash, hash);
            files_owned[i].chunks[j].index = j;
        }
    }

    // Trimite numărul total de fișiere
    MPI_Send(&files_owned_count, 1, MPI_INT, TRACKER_RANK, FILE_TAG, MPI_COMM_WORLD);
    
    // Trimite informațiile despre fișiere
    MPI_Send(file_info, sizeof(file_info_request) * files_owned_count, MPI_BYTE, TRACKER_RANK, FILE_TAG, MPI_COMM_WORLD);

    for (int i = 0; i < files_owned_count; i++) {
        for (int j = 0; j < file_info[i].chunks; j++) {
            MPI_Send(files_owned[i].chunks[j].hash, HASH_SIZE, MPI_CHAR, TRACKER_RANK, FILE_TAG, MPI_COMM_WORLD);
        }
    }

    fscanf(input, "%d", &nr_req_files);
    for (int i = 0; i < nr_req_files; i++) {
        fscanf(input, "%s", requested_files[i].filename);
        requested_files[i].nr_chunks_recv = 0;
        requested_files[i].is_owned = false;

        for (int j = 0; j < MAX_CHUNKS; j++) {
            requested_files[i].chunks_recv[j] = -1;
        }
    }

    fclose(input);

    // Așteaptă confirmarea trackerului
    char answer[15];
    MPI_Recv(answer, 15, MPI_CHAR, TRACKER_RANK, FILE_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    if (strcmp(answer, "ACK") == 0) {
        printf("ACK from %d\n", rank);
    } else {
        printf("NACK from %d\n", rank);
        exit(-1);
    }
}


void peer(int numtasks, int rank) {
    process_owned_files(rank);

    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;
    int r;

    r = pthread_create(&download_thread, NULL, download_thread_func, (void *) &rank);
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
        tracker_func(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();
}