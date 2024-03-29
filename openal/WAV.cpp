/*
 * WAV.cpp
 *
 *  Created on: Dec 12, 2014
 *      Author: rami
 */

#include "WAV.h"

int WAV::play(string fileName) {
	ALCdevice *dev;
	ALCcontext *ctx;
	struct stat statbuf;

	/* First the standard open-device, create-context, set-context.. */
	dev = alcOpenDevice(NULL);
	if (!dev) {
		fprintf(stderr, "Oops\n");
		return 1;
	}
	ctx = alcCreateContext(dev, NULL);
	alcMakeContextCurrent(ctx);
	if (!ctx) {
		fprintf(stderr, "Oops2\n");
		return 1;
	}

	{
		/* The number of buffers and bytes-per-buffer for our stream are set
		 * here. The number of buffers should be two or more, and the buffer
		 * size should be a multiple of the frame size (by default, OpenAL's
		 * largest frame size is 4, however extensions that can add more formats
		 * may be larger). Slower systems may need more buffers/larger buffer
		 * sizes. */
		/* These are what we'll use for OpenAL playback */
		ALuint source, buffers[NUM_BUFFERS];
		ALuint frequency;
		ALenum format;
		unsigned char *buf;
		/* These are used for interacting with mplayer */
		int pid, files[2];
		FILE *f;

		/* Generate the buffers and sources */
		alGenBuffers(NUM_BUFFERS, buffers);
		alGenSources(1, &source);
		if (alGetError() != AL_NO_ERROR) {
			fprintf(stderr, "Error generating :(\n");
			return 1;
		}

		/* Here's where our magic begins. First, we want to call stat on the
		 * filename since mplayer will just silently exit if it tries to play a
		 * non-existant file **/
		if (stat(fileName.c_str(), &statbuf) != 0 || !S_ISREG(statbuf.st_mode)) {
			fprintf(stderr, "%s doesn't seem to be a regular file :(\n",
					fileName.c_str());
			return 1;
		}
		/* Open a file pipe. This will create two file-descriptors, one for
		 * reading and another for writing. The data will be passed in memory,
		 * so it won't be bogged by disk access. */
		if (pipe(files) != 0) {
			fprintf(stderr, "Pipe failed :(\n");
			return 1;
		}

		/* Now we fork. The forked process will inherit the original process's
		 * file descriptors, so each process will have access to the same pipe.
		 * Note that the process memory isn't shared (if you change something in
		 * one process, the other will be unaffected). */
		pid = fork();
		switch (pid) {
		case -1:
			/* If it returns -1, there was an error */
			fprintf(stderr, "Fork failed :(\n");
			return 1;
			break;

		case 0:
			/* Returning 0 means that we're now in the child process, that
			 * we'll turn into mplayer. First, we can close the read file
			 * descriptor since this process won't be reading from it. */
			close(files[0]);

			/* Here's part of the trick. After closing the stdout file
			 * descriptor, dup2 assigns it the pipe's write file descriptor.
			 * So now, whenever anything writes to stdout, it'll go to the
			 * pipe instead! */
			close(STDOUT_FILENO);
			dup2(files[1], STDOUT_FILENO);

			/* We can use execlp to run mplayer with the options we need. To
			 * output audio as a standard .wav-formatted file, we use the
			 * pcm audio-out device, and tell it to write to stdout. By
			 * running this, we overwrite the current process memory with
			 * the named commmand, which causes it to start mplayer with the
			 * overridden stdout */
			execlp("mplayer", "-nogui", "-really-quiet", "-novideo",
					"-noconsolecontrols", "-ao", "pcm:file=/dev/stdout",
					fileName.c_str(), (char*) NULL);
			/* The exec* functions should never return. If it does,
			 * something went wrong, so just _exit. */
			_exit(1);
		default:
			/* Any other return value means we're in the parent process.
			 * Here, we don't need the write file descriptor, so close it.
			 * Now we can begin using the read file descriptor to read
			 * mplayer's stdout, which will be the file decoded in real-
			 * time! */
			close(files[1]);
			break;
		}

		/* fdopen simply creates a FILE* from the given file descriptor. This is
		 * generally easier to work with, but there's no reason you couldn't use
		 * the lower-level io routines on the descriptor if you wanted */
		f = fdopen(files[0], "rb");

		/* Allocate the buffer, and read the RIFF-WAVE header. We don't actually
		 * need to read it, so just ignore what it writes to the buffer. Because
		 * this is a file pipe, it is unseekable, so we have to read bytes we
		 * want to skip. Also note that because mplayer is writing out the file
		 * in real-time, the chunk size information may not be filled out. */
		buf = (unsigned char*) malloc(BUFFER_SIZE);
		fread(buf, 1, 12, f);

		/* This is the first .wav file chunk. Check the chunk header to make
		 * sure it is the format information. The first four bytes is the
		 * indentifier (which we check), and the last four is the chunk size
		 * (which we ignore) */
		fread(buf, 1, 8, f);
		if (buf[0] != 'f' || buf[1] != 'm' || buf[2] != 't' || buf[3] != ' ') {
			/* If this isn't the format info, it probably means it was an
			 * unsupported audio format for mplayer, or the file didn't contain
			 * an audio track. */
			fprintf(stderr, "Not 'fmt ' :(\n");
			/* Note that closing the file will leave mplayer's write file
			 * descriptor without a read counterpart. This will cause mplayer to
			 * receive a SIGPIPE signal, which will cause it to abort and exit
			 * automatically for us. Alternatively, you can use the pid returned
			 * from fork() to send it a signal explicitly. */
			fclose(f);
			return 1;
		}

		{
			int channels, bits;

			/* Read the wave format type, as a 16-bit little-endian integer.
			 * There's no reason this shouldn't be 1. */
			fread(buf, 1, 2, f);
			if (buf[1] != 0 || buf[0] != 1) {
				fprintf(stderr, "Not PCM :(\n");
				fclose(f);
				return 1;
			}

			/* Get the channel count (16-bit little-endian) */
			fread(buf, 1, 2, f);
			channels = buf[1] << 8;
			channels |= buf[0];

			/* Get the sample frequency (32-bit little-endian) */
			fread(buf, 1, 4, f);
			frequency = buf[3] << 24;
			frequency |= buf[2] << 16;
			frequency |= buf[1] << 8;
			frequency |= buf[0];

			/* The next 6 bytes hold the block size and bytes-per-second. We
			 * don't need that info, so just read and ignore it. */
			fread(buf, 1, 6, f);

			/* Get the bit depth (16-bit little-endian) */
			fread(buf, 1, 2, f);
			bits = buf[1] << 8;
			bits |= buf[0];

			/* Now convert the given channel count and bit depth into an OpenAL
			 * format. We could use extensions to support more formats (eg.
			 * surround sound, floating-point samples), but that is beyond the
			 * scope of this tutorial */
			format = 0;
			if (bits == 8) {
				if (channels == 1)
					format = AL_FORMAT_MONO8;
				else if (channels == 2)
					format = AL_FORMAT_STEREO8;
			} else if (bits == 16) {
				if (channels == 1)
					format = AL_FORMAT_MONO16;
				else if (channels == 2)
					format = AL_FORMAT_STEREO16;
			}
			if (!format) {
				fprintf(stderr, "Incompatible format (%d, %d) :(\n", channels,
						bits);
				fclose(f);
				return 1;
			}
		}

		/* Next up is the data chunk, which will hold the decoded sample data */
		fread(buf, 1, 8, f);
		if (buf[0] != 'd' || buf[1] != 'a' || buf[2] != 't' || buf[3] != 'a') {
			fclose(f);
			fprintf(stderr, "Not 'data' :(\n");
			return 1;
		}

		/* Now we have everything we need. To read the decoded data, all we have
		 * to do is read from the file handle! Note that the .wav format spec
		 * has multibyte sample foramts stored as little-endian. If you were on
		 * a big-endian machine, you'd have to iterate over the returned data
		 * and flip the bytes for those formats before giving it to OpenAL. Also
		 * be aware that there is no seeking on the file handle. A slightly more
		 * complex setup could be made to send commands back to mplayer to seek
		 * on the stream, however that is beyond the scope of this tutorial. */
		{
			int ret;

			/* Fill the data buffer with the amount of bytes-per-buffer, and
			 * buffer it into OpenAL. This may read (and return) less than the
			 * requested amount when it hits the end of the "stream" */
			ret = fread(buf, 1, BUFFER_SIZE, f);
			alBufferData(buffers[0], format, buf, ret, frequency);

			/* Once the data's buffered into OpenAL, we're free to modify our
			 * data buffer, so reuse it to fill the remaining OpenAL buffers. */
			ret = fread(buf, 1, BUFFER_SIZE, f);
			alBufferData(buffers[1], format, buf, ret, frequency);
			ret = fread(buf, 1, BUFFER_SIZE, f);
			alBufferData(buffers[2], format, buf, ret, frequency);
			if (alGetError() != AL_NO_ERROR) {
				fprintf(stderr, "Error loading :(\n");
				return 1;
			}

			/* Queue the buffers onto the source, and start playback! */
			alSourceQueueBuffers(source, NUM_BUFFERS, buffers);
			alSourcePlay(source);
			if (alGetError() != AL_NO_ERROR) {
				fprintf(stderr, "Error starting :(\n");
				return 1;
			}

			/* While not at the end of the stream... */
			while (!feof(f)) {
				ALuint buffer;
				ALint val;

				/* Check if OpenAL is done with any of the queued buffers */
				alGetSourcei(source, AL_BUFFERS_PROCESSED, &val);
				if (val <= 0)
					continue;

				/* For each processed buffer... */
				while (val--) {
					/* Read the next chunk of decoded data from the stream */
					ret = fread(buf, 1, BUFFER_SIZE, f);

					/* Pop the oldest queued buffer from the source, fill it
					 * with the new data, then requeue it */
					alSourceUnqueueBuffers(source, 1, &buffer);
					alBufferData(buffer, format, buf, ret, frequency);
					alSourceQueueBuffers(source, 1, &buffer);
					if (alGetError() != AL_NO_ERROR) {
						fprintf(stderr, "Error buffering :(\n");
						return 1;
					}
				}
				/* Make sure the source is still playing, and restart it if
				 * needed. */
				alGetSourcei(source, AL_SOURCE_STATE, &val);
				if (val != AL_PLAYING)
					alSourcePlay(source);
			}
		}

		/* File's done decoding. We can close the pipe and free the data buffer
		 * now. */
		fclose(f);
		free(buf);
		{
			ALint val;
			/* Although mplayer is done giving us data, OpenAL may still be
			 * playing the remaining buffers. Wait until it stops. */
			do {
				alGetSourcei(source, AL_SOURCE_STATE, &val);
			} while (val == AL_PLAYING);
		}

		/* Done playing. Delete the source and buffers */
		alDeleteSources(1, &source);
		alDeleteBuffers(NUM_BUFFERS, buffers);
	}

	/* All done. Close OpenAL and exit. */
	alcMakeContextCurrent(NULL);
	alcDestroyContext(ctx);
	alcCloseDevice(dev);

	return 0;
}


int WAV::play2(string fileName) {
	ALCcontext *context;
	ALCdevice *device;

	device = alcOpenDevice(NULL);
	if (device == NULL)
	{
	   // Handle Exception
		cerr << "No device?" << endl;
	}

	//Create a context
	context=alcCreateContext(device,NULL);

	//Set active context
	alcMakeContextCurrent(context);

	// Clear Error Code
	cerr << alGetError() << endl;

	ALenum alFormatBuffer;    //buffer format
	char* alBuffer;             //data for the buffer
	ALsizei alBufferLen;        //bit depth
	ALsizei alFreqBuffer;       //frequency
	ALboolean    alLoop;         //loop

	unsigned int alSource;      //source
	unsigned int alSampleSet;

	//load the wave file
	alutLoadWAVFile((ALbyte*) fileName.c_str(), &alFormatBuffer, (void**) &alBuffer, &alBufferLen, &alFreqBuffer, &alLoop);

	//create a source
	alGenSources(1, &alSource);

	//create  buffer
	alGenBuffers(1, &alSampleSet);

	//put the data into our sampleset buffer
	alBufferData(alSampleSet, alFormatBuffer, alBuffer, alBufferLen, alFreqBuffer);

	//assign the buffer to this source
	alSourcei(alSource, AL_BUFFER, alSampleSet);

	//release the data
	alutUnloadWAV(alFormatBuffer, alBuffer, alBufferLen, alFreqBuffer);

	cerr << alFormatBuffer << ' ' << alBufferLen << ' ' << alFreqBuffer << endl;

	alSourcei(alSource,AL_LOOPING,AL_TRUE);

	//play
	alSourcePlay(alSource);

	//to stop
//	alSourceStop(alSource);

//	alDeleteSources(1,&alSource);

	//delete our buffer
//	alDeleteBuffers(1,&alSampleSet);

	context=alcGetCurrentContext();

	//Get device for active context
	device=alcGetContextsDevice(context);

	//Disable context
//	alcMakeContextCurrent(NULL);

	//Release context(s)
//	alcDestroyContext(context);

	//Close device
//	alcCloseDevice(device);
}
