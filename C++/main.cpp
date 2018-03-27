#include <stdio.h>
#include <stdlib.h>
#include <thread>
#include <mutex> 
#include <condition_variable>
#include <csignal>
#include <string>
#include <iostream>
#include <sstream>
#include <map>
#include <iomanip>
#include <algorithm>
#include <iterator>
#include <vector>
#include <unistd.h>
#include <sys/types.h>
#include <errno.h>
#include <chrono>
#include <ctime>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "config.h"
#include "spdlog/spdlog.h"

using namespace std;
/*
typedef struct {
	int t;
} ffmpeg_manager_struct;
*/

extern int errno;


mutex config_mutex;
mutex t_mutex;
mutex proc_mutex;
mutex stop_mutex;
condition_variable stop_cv;

time_t last_checked;
pid_t ffmpeg_proc = -1;
int stop = 0;


/*
** Catch USRSIG1 and kill current ffmpeg process.
** Usually used when t has been changed in config file
** and you want immediate effect, rather than after current
** video is finished (default).
*/ 
void catch_SIGUSR(int signo) {
	auto capture = spdlog::get("capture");
	capture->info("Received SIGUSR, stopping ffmpeg process");
	proc_mutex.lock();
	stop_mutex.lock();
	if(ffmpeg_proc > 0){
		kill(ffmpeg_proc,SIGKILL);
	}
	
	if(signo == SIGUSR1)
		stop = 0;
	else
		stop = 1 - stop;
		
	stop_cv.notify_one();
		
	capture->info("Setting stop to " + stop);
	
	stop_mutex.unlock();
	proc_mutex.unlock();
}



// Set camera parameters when changed in config file
int setup_camera(){
	auto capture = spdlog::get("capture");
	config_mutex.lock();

	try {
		map<string,double> cam_settings = camera_config();	

		config_mutex.unlock();
		ostringstream sStream;

		sStream << "v4l2-ctl";

		for (auto& setting : cam_settings)
			sStream << " -c " << setting.first << "=" << (int)setting.second;
	
		string tmp = sStream.str();
		const char * cmd = tmp.c_str();
	
		system(cmd);
	} catch (const configNotFoundException& e) {
		capture->error("Not all camera parameters in config file");
		return -1;
	} catch (const exception& e){
		capture->error(e.what()); 
		return -1;
	} 

	return 0;

}

// Function from which thread manages the ffmpeg process
void ffmpeg_process_manager(int t){
	auto capture = spdlog::get("capture");
	int status = 0;
	string ffmpeg_cmd[] = {"/usr/bin/ffmpeg","-y","-i","/dev/video1","-s","1280x720",
		"-vcodec","copy","-t",to_string(t),"",
		"-t",to_string(t),"-vf", "fps=1/5",
		"-update","1","../Network/frame.jpg",
		"-vcodec","copy","-t",to_string(t),"-f","flv","rtmp://mousehotelserver.inf.ed.ac.uk::8080/live/test"};
		
	time_t rawt;
	pid_t pid;
	struct tm *lt;	
	struct stat config_stat;

	while(1){
		unique_lock<mutex> stop_lock(stop_mutex);
		while(stop)
			stop_cv.wait(stop_lock);
		stop_lock.unlock();

		// Check config hasn't been modified
		if(stat(CONFIG,&config_stat)<0){
			capture->warn("Error using stat to check last access of config file");
		} else {
			if(difftime(config_stat.st_mtime,last_checked)>0){
				last_checked = config_stat.st_mtime;
				try{
					t = read_config("Time") * 60;
				} catch (const configNotFoundException& e){
					capture->warn(e.what());
					capture->info("Using previous value of " + t);
				} catch (const exception& e){
					capture->error(e.what());
					exit(1);
				}
				//setup_camera();
			}
		}



		time(&rawt);
		lt = localtime(&rawt);
		ostringstream sStream;
		sStream << "Video/";
		sStream << lt->tm_year+1900 << "-";
		sStream	<< setfill('0') << setw(2)
			<< lt->tm_mon+1 << "-";
		sStream	<< setfill('0') << setw(2)
			<< lt->tm_mday << "_";
		sStream	<< setfill('0') << setw(2)
			<< lt->tm_hour << ":";
		sStream	<< setfill('0') << setw(2)
			<< lt->tm_min << ":";
		sStream	<< setfill('0') << setw(2)
			<< lt->tm_sec << ".flv";

		string filename = sStream.str();		

		capture->info("Filename set as " + filename);
		// rewrite time and filename
		ffmpeg_cmd[9] = ffmpeg_cmd[12] = to_string(t);
		ffmpeg_cmd[10] = filename;

		// Convert to char* for execv
		vector<char*> cmd_v;

		// add null terminators
		transform(begin(ffmpeg_cmd),end(ffmpeg_cmd),
			back_inserter(cmd_v),
			[](string& s){ s.push_back(0); return &s[0];});
		cmd_v.push_back(nullptr);

		char** c_cmd = cmd_v.data();

		if (!(pid=fork())) {
			int out_file = open("ffmpeg.log",O_RDWR|O_CREAT|O_TRUNC,0666);
			if(out_file < 0){
				capture->error("Error opening ffmpeg.log");
				exit(1);
			}
			
			if(dup2(out_file,STDOUT_FILENO) < 0){
				capture->error("Error using dup2 to redirect stdout of ffmpeg process to ffmpeg.log");
				exit(1);
			}
			if(dup2(out_file,STDERR_FILENO) < 0){
				capture->error("Error using dup2 to redirect stderr of ffmpeg process to ffmpeg.log");
				exit(1);
			}


			capture->info("Executing ffmpeg");
			if(execv("/usr/bin/ffmpeg",c_cmd) < 0){
				capture->error("Error using execv to start ffmpeg");
				exit(1);
			}
			exit(0);
		} else {
			proc_mutex.lock();
			ffmpeg_proc = pid;
			proc_mutex.unlock();

			wait(&status);

			proc_mutex.lock();
			ffmpeg_proc = -1;
			proc_mutex.unlock();

			capture->info("Recording complete");

			if(WIFEXITED(status))
				if(WEXITSTATUS(status) > 0){
					capture->warn("Ffmpeg returned with an error, stopping recording");
					capture->warn("Run ffmpeg natively to debug");
					stop_mutex.lock();
					stop = 1;
					stop_mutex.unlock();
				}
		}
	}
	
}


int main(int argc, char *argv[]){
	double t = 15;
	int new_t = 0;
	string str;
	struct stat config_stat;
    	auto capture = spdlog::basic_logger_mt("capture","logs/capture.log");

	if (argc > 1){
		t = strtod(argv[1],NULL);
		new_t = 1;
	}


	capture->info("Starting setup");


	int out_file = open("logs/capture.log",O_RDWR|O_CREAT|O_TRUNC,0666);
	if(out_file < 0){
		capture->error("Error opening capture.log");
		exit(1);
	}

	
	if(dup2(out_file,STDOUT_FILENO) < 0){
		capture->error("Error using dup2 to redirect stdout to capture.log");
		exit(1);
	}
	if(dup2(out_file,STDERR_FILENO) < 0){
		capture->error("Error using dup2 to redirect stderr to capture.log");
		exit(1);
	}
	
	ConfigPair toSave;
	toSave.option = "Time"; toSave.value = t;

	config_mutex.lock();

	try {
		if(new_t)
			edit_config(toSave); 
		else
			t = read_config("Time");

	} catch (const configOpenException& e){
		create_config(t);
	} catch (const configNotFoundException& e){
		capture->warn(e.what());
		capture->info("Adding Time as " + to_string(t));
		add_config(toSave);
	} catch (const exception& e){
		capture->error(e.what());
		exit(1);
	}
	
	toSave.option = "pid"; toSave.value = getpid();

	try {
		edit_config(toSave); 
	} catch (const configNotFoundException& e){
		add_config(toSave);
	} catch (const exception& e){
		capture->error(e.what());
		exit(1);
	}

	config_mutex.unlock();

//	if (setup_camera() < 0) {
//		capture->error("Error setting up camera");
//		exit(1);
//	}

	if (signal(SIGUSR1,catch_SIGUSR) == SIG_ERR)
		capture->error("Error setting USRSIG1");

	if (signal(SIGUSR2,catch_SIGUSR) == SIG_ERR)
		capture->error("Error setting USRSIG1");

	t *= 60;

	if(stat(CONFIG,&config_stat)<0){
		capture->error("Error using stat to check last access of config file");
		exit(1);
	}

	last_checked = config_stat.st_mtime;

	capture->info("Setup complete, starting ffmpeg_process_manager thread");

	thread manager(ffmpeg_process_manager,t);

	// Posibly do things here relating to image processing

	manager.join();
	

	return 0;
}
