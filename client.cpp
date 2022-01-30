#include "common.h"
#include "BoundedBuffer.h"
#include "Histogram.h"
#include "HistogramCollection.h"
#include "FIFOreqchannel.h"

using namespace std;

/* Default values */
int opt;
int n = 100;    		// default number of requests per "patient"
int p = 10;     		// number of patients [1,15]
int w = 100;    		// default number of worker threads
int b = 20; 		    // default capacity of the request buffer, you should change this default
int m = MAX_MESSAGE; 	// default capacity of the message buffer
string filename;        // file name
int h = 1;              // default histogram thread amount
int file_size = 0;      // contains file size
bool f_flag = false;    // file request flag

FIFORequestChannel* newChannel(FIFORequestChannel* chan) {
	MESSAGE_TYPE nm = NEWCHANNEL_MSG;
	chan->cwrite(&nm, sizeof(MESSAGE_TYPE));
	// Getting the new channel's name
	char name[30];
	chan->cread(&name, sizeof(name));
	// Access the new channel
	FIFORequestChannel* new_chan = new FIFORequestChannel(name, FIFORequestChannel::CLIENT_SIDE);
	return new_chan;
}

void closeChannel(FIFORequestChannel* chan) {
	/* Closing the channel */    
	MESSAGE_TYPE qm = QUIT_MSG;
	chan->cwrite(&qm, sizeof(MESSAGE_TYPE));
    delete chan;
	return;
}

void patientThreadFunction(BoundedBuffer& request_buffer, int patient) {
    /* Push n data messages to the request buffer */
    datamsg dm(patient, 0, 1);
    for (int i = 0; i < n; i++) {
        // Push request for ecg 1 value
        dm.seconds = i*0.004;
        char buf[sizeof(datamsg)];
        memcpy(buf, &dm, sizeof(datamsg));
        request_buffer.push(buf, sizeof(datamsg));
    }
}

void workerThreadFunction(BoundedBuffer& request_buffer, BoundedBuffer& response_buffer, FIFORequestChannel* w_chan) {
    /* Worker processes requests until there are no more */
    while (true) {
        // Pop a request from the request buffer
        int len = sizeof(filemsg) + filename.size() + 1;
        char request[256];
        request_buffer.pop(request, 256);
        MESSAGE_TYPE msg = *(MESSAGE_TYPE*)request;
        char response[m];
        if (msg == FILE_MSG) { // Client requesting files
            filemsg fm = *(filemsg*)request;
            w_chan->cwrite(request, len); // Send a filemsg
            w_chan->cread(response, m); // Receive m bytes
            //cout << fm.offset << " " << fm.length << endl;

            FILE* fp = fopen(("received/" + filename).c_str(), "rb+");
            fseek(fp, fm.offset, SEEK_SET);
            fwrite(response, sizeof(char), fm.length, fp);
            fclose(fp);
        }
        else if (msg == DATA_MSG) { // Client requesting data points
            datamsg dm = *(datamsg*)request;
            w_chan->cwrite(request, sizeof(datamsg)); // Send a datamsg
            w_chan->cread(response, sizeof(double)); // Receive a double
            double ecg1 = *(double*)response;
            // Push the response to the response buffer
            char buf[sizeof(pair<int, double>)];
            pair<int, double> data = make_pair(dm.person, ecg1);
            memcpy(buf, &data, sizeof(pair<int, double>));
            response_buffer.push(buf, sizeof(pair<int, double>));
        }
        else if (msg == QUIT_MSG) { // Worker needs to quit
            break;
        }
        else { // unknown message?
            w_chan->cwrite(request, len);
            w_chan->cread(response, m);
        }
    }
    closeChannel(w_chan);
    return;
}

void histogramThreadFunction(BoundedBuffer& response_buffer, HistogramCollection& hc) {
    /* Histogram threads updates appropriate histogram with new value until there are no more */
    while (true) {
        // Pop a response from the response buffer
        char response[sizeof(pair<int, double>)];
        response_buffer.pop(response, sizeof(pair<int, double>));
        pair<int, double> data = *(pair<int, double>*)response;
        int patient = data.first;
        double ecg1 = data.second;
        if (patient >= 1 && patient <= 15) {
            hc.update(patient, ecg1);
        }
        else {
            break;
        }
    }
    return;
}

void fileRequestThreadFunction(BoundedBuffer& request_buffer, FIFORequestChannel* control_chan) {
    /* Push as many file requests as needed to transfer a file */
    // First get the file length in bytes
	filemsg fm(0,0);
	int len = sizeof(filemsg) + filename.size() + 1;
	char buf[len];
	memcpy(buf, &fm, sizeof(filemsg)); // copy filemsg to buf
	strcpy(buf + sizeof(filemsg), filename.c_str()); // copy fname to file
	control_chan->cwrite(buf, len);
	control_chan->cread(&file_size, sizeof(__int64_t)); // read in file size

    // Allocate file in memory
    FILE* fp;
    if ((fp = fopen(("received/" + filename).c_str(), "wb")) == NULL) {
        perror("open");
        exit(1);
    }
    fseek(fp, file_size, SEEK_SET);
    fclose(fp);
    
    // Push file requests to buffer, chunk by chunk
    char buf2[len];
    filemsg fm2(0,0);
	for (int i = 0; i < ceil(double(file_size)/m); i++) {
		if (file_size - fm2.offset < m) { // "Leftover" bytes statement
			fm2.length = file_size - fm2.offset;
		}
		else {
            fm2.length = m;
		}
        memcpy(buf2, &fm2, sizeof(filemsg)); // copy filemsg to buf2
        strcpy(buf2 + sizeof(filemsg), filename.c_str()); // copy fname to file
        request_buffer.push(buf2, len);
		fm2.offset += fm2.length;
	}
    return;
}

int main(int argc, char *argv[]) {
    srand(time_t(NULL));
    
    while ((opt = getopt(argc, argv, "n:p:w:b:m:f:h:")) != -1) {
		switch (opt) {
            case 'n':
                n = atoi(optarg);
                break;
            case 'p':
                p = atoi(optarg);
                break;
            case 'w':
                w = atoi(optarg);
                break;
            case 'b':
                b = atoi(optarg);
                break;
            case 'm':
                m = atoi(optarg);
                break;
            case 'f':
                f_flag = true;
                filename = optarg;
                break;
            case 'h':
                h = atoi(optarg);
                break;
		}
	}
    
    int pid = fork();
    if (pid == 0) { // Server
        execv("./server", argv);
    }
    else { // Client
        FIFORequestChannel* chan = new FIFORequestChannel("control", FIFORequestChannel::CLIENT_SIDE);
        BoundedBuffer request_buffer(b);
        BoundedBuffer response_buffer(b);
        HistogramCollection hc;
        
        // Timing stuff
        struct timeval start, end;
        gettimeofday(&start, 0);

        // Make all the worker channels first
        FIFORequestChannel* w_channels[w];
        for (int i = 0; i < w; i++)
            w_channels[i] = newChannel(chan);
        
        if (f_flag) { // Client requesting file
            /* Start all threads here */
            thread file_request(fileRequestThreadFunction, ref(request_buffer), chan); // spawn file request thread
            thread w_threads[w]; // store worker threads
            for (int i = 0; i < w; i++) // spawn worker threads
                w_threads[i] = thread(workerThreadFunction, ref(request_buffer), ref(response_buffer), w_channels[i]);

            /* Join all threads here */
            file_request.join();
            for (int i = 0; i < w; i++) { // tell all workers to quit
                MESSAGE_TYPE qm = QUIT_MSG;
                request_buffer.push((char*)&qm, sizeof(MESSAGE_TYPE));
            }
            for (thread& i : w_threads) // tell all workers to join
                i.join();
            
        }
        else { // Client requesting data points
            /* Create histograms */
            for (int i = 0; i < p; i++) {
                Histogram* hist = new Histogram(30, -1.5, 1.5);
                hc.add(hist); // histogram with 30 bins, range is [-1.5,1.5]
            }

            /* Start all threads here */
            thread p_threads[p]; // store patient threads
            thread w_threads[w]; // store worker threads
            thread h_threads[h]; // store histogram threads
            for (int i = 0; i < p; i++) // patient threads
                p_threads[i] = thread(patientThreadFunction, ref(request_buffer), i + 1);
            for (int i = 0; i < w; i++) // worker threads
                w_threads[i] = thread(workerThreadFunction, ref(request_buffer), ref(response_buffer), w_channels[i]);
            for (int i = 0; i < h; i++) // histogram threads
                h_threads[i] = thread(histogramThreadFunction, ref(response_buffer), ref(hc));

            /* Join all threads here */
            for (thread& i : p_threads)
                i.join();
            for (int i = 0; i < w; i++) { // tell all workers to quit
                MESSAGE_TYPE qm = QUIT_MSG;
                request_buffer.push((char*)&qm, sizeof(MESSAGE_TYPE));
            }
            for (thread& i : w_threads)
                i.join();
            for (int i = 0; i < h; i++) { // tell all histogram threads to quit
                pair<int, double> quit = make_pair(0, 0);
                response_buffer.push((char*)&quit, sizeof(pair<int, double>));
            }
            for (thread& i : h_threads)
                i.join();
        }

        gettimeofday(&end, 0);
        // print the results
        hc.print();
        int secs = (end.tv_sec * 1e6 + end.tv_usec - start.tv_sec * 1e6 - start.tv_usec)/(int)1e6;
        int usecs = (int)(end.tv_sec * 1e6 + end.tv_usec - start.tv_sec * 1e6 - start.tv_usec)%((int)1e6);
        cout << "Took " << secs << " seconds and " << usecs << " micro seconds" << endl;

        closeChannel(chan);
        cout << "All Done!!!" << endl;
    }    
}
