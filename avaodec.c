/*

	LAVC Boilerplate

	A boilerplate program for doing stuff with libavcodec and FFTW.
	Uses libao for audio output: configure that in ~/.libao

	(c)2015 Chris Northcott

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <errno.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <signal.h>
#include <fftw3.h>
#include <math.h>
#include <libavutil/mathematics.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <ao/ao.h>

#define FFT_WINSZ_IN 	1024
#define FFT_WINSZ_OUT	(FFT_WINSZ_IN / 2) + 1

#define SCREEN_WIDTH 640
#define SCREEN_HEIGHT 480

#define BAR_WIDTH 4

#define FONT "cant.otf", 12

SDL_Window *window;
SDL_Surface *surface;
SDL_Renderer *renderer;
TTF_Font *cant;

AVFormatContext *container;
AVCodecContext *context;
AVCodec *codec;
AVDictionary *dictionary;
AVPacket p;
AVFrame *f;

char inputfilename[255];
char *title;
char *artist;
int activestreamindex = 0;
int bufsz, len, eoframe;
uint8_t *buffer;

int nframes = 0;

fftw_plan plan;
double *fft_in;
fftw_complex *fft_out;

float rollingaccum;

int driver;
ao_sample_format aosampleformat;
ao_device *dev = NULL;

void logmsg(char type, const char* msg)
{
	printf("[%c%c] %s\r\n", type, type, msg);
	if(type == 'e') exit(-1);
}

void SIGINT_handler(int param)
{
	logmsg('n', "Exit requested. Bye!");
	exit(0);
}

void do_fft(short *buffer, int buffersize)
{

	/*
		The meat of the program.
	*/

	//Normalise values
	for(int i = 0; i < buffersize; i++)
	{
		float x = (float)buffer[i] * (1.0f / 32768.0f);
		if(i < FFT_WINSZ_IN) fft_in[i] = x;
	}

	//Run FFT
	fftw_execute(plan);

	//Declare an array of floats to hold our power spectum values.
	float *powerspectrum = malloc(sizeof(float) * FFT_WINSZ_IN);
	
	//For each value in the FFT output...
	for(int i = 0; i < FFT_WINSZ_IN; i++)
	{
		/*
			Turn it into a magnitude.
			This is the square root of the sum of the squared
			Real and Imaginary parts of the FFT output, or

			A(mag) = sqrt(Xn[Re]^2 + Xn[Im]^2)

			We could also extract phase at this point with

			theta = atan(Xn[Re]^2)

			but our visualisation doesn't require it. WMP's
			visualisations do use phase as a rotation coefficient
			for e.g. Dance of the Freaky Circles.
		*/
		float mag = sqrtf((fft_out[i][0] * fft_out[i][0]) +
				(fft_out[i][1] * fft_out[i][1]));

		/*
			Turn the magnitude into a logarithmic power reading.

			P(dB) = 20log10(mag^2)
		*/
		float dB = (20 * log10(mag * mag));
	
		/*
			Add this value to the power spectrum.
		*/	
		powerspectrum[i] = abs(dB);
	}

	//Clear the background to black.
        SDL_FillRect(surface, NULL, SDL_MapRGB(surface->format, 0, 0 ,0));
	SDL_SetRenderDrawColor( renderer, 0, 0, 0, 0xFF ); 
	SDL_RenderClear( renderer );

	//'scale' is used as a scaling factor to emphasise lower frequencies.
	float scale = 6.0f;

	//'accum' is used to calculate an average power reading (for later 
	// implementation of a beat detector).
	float accum = 0.0f;

	for(int i = 0; i < FFT_WINSZ_IN; i++)
	{
		//Scale the final displayed value according to 'scale'.
		// This decreases as the number of processed readings increases.
		float val = powerspectrum[i] * scale;
		scale = scale - 0.02f;

		//We add to the accumulator for later use for averaging
		if(i < 40){
			accum = accum + val;
		}

		//Throw away any power spectrum readings that are off the screen.
		if(i > SCREEN_WIDTH) continue;

		//colour based on amplitude
		int shade = 255 * (255 / val);
		SDL_SetRenderDrawColor(renderer, 0, shade, 0, 0xff);
	
		//calculate dimensions of this "bar" and render it
		SDL_Rect r;
		r.x = i * BAR_WIDTH;
		r.y = SCREEN_HEIGHT - val;
		r.w = BAR_WIDTH;
		r.h = val;
		SDL_RenderFillRect(renderer, &r); 
	}

	//Keep a rolling total to average out across all frames.
	//TODO: this should be a rolling window of the last `n` samples, not
	//  averaged out over the entire song.
	rollingaccum += accum;

	//Calculate the actual averages.
	float A = accum / 40;
	float RA = (rollingaccum / nframes) /40;

	char *statusline = malloc(sizeof(char) * 255);
	sprintf(statusline, "%s - %s", artist, title);

	/*
		Draw a little red box if the power average for this frame
		is higher than the average for the whole song
	*/
	if(A > RA)
	{
		SDL_Rect r = { 1, 1, 5, 5};
		SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
		SDL_RenderFillRect(renderer, &r);
	}
	
	//Render a status line with the currently playing song
	SDL_Color c = { 255, 255, 255, 255 };
	SDL_Surface *ttf_s = TTF_RenderText_Blended(cant, statusline, c);
	SDL_Texture *ttf_t = SDL_CreateTextureFromSurface(renderer, ttf_s);
	int txw, txh;
	SDL_QueryTexture(ttf_t, NULL, NULL, &txw, &txh);
	SDL_Rect statuslinerect = { 5, 5, txw, txh };
	SDL_RenderCopy(renderer, ttf_t, NULL, &statuslinerect);
	SDL_FreeSurface(ttf_s);
	SDL_DestroyTexture(ttf_t);

	//and render the contents of the current rendering target
	SDL_RenderPresent( renderer );
}

void play(void)
{
	len = eoframe = 0;

	//Get avcodec to read out a single frame.
	while(av_read_frame(container, &p) >= 0)
	{
		//If this frame belongs to the audio stream we selected earlier;
		if(p.stream_index == activestreamindex)
		{
			//decode packet 'p' into frame 'f' and store the amount
			// of decoded bytes in 'len'.
			// Set "eoframe" if we have decoded a whole frame.
			len = avcodec_decode_audio4(context, f, &eoframe, &p);
			if(len < 0) 
				logmsg('w', "Decoding error!");
			if(eoframe) //If we have recieved a whole frame...
			{
				//that's one more for the team
				nframes++;
			
				//Play the actual sample via the sound card.
				ao_play(dev, (char *)f->extended_data[0],
					f->linesize[0]);

				//do_fft casts to "short *" because we are expecting
				// buffer of 16-bit signed numbers for the FFT, rather
				// than a bytestream like AO does.
				do_fft((short *)f->extended_data[0],
					f->linesize[0]);			
			}
		}else{
			logmsg('w', "Trying to play something that isn't audio...?");
		}
	}

}

void init_SDL(void)
{
	/*
		Initialise our graphical output.

		This is all really basic stuff; SDL is pretty self explanatory.
		No error checking, but if we can't get graphical output we're
		f*cked anyway and a segfault is the least of our worries.
	*/

	if(SDL_Init(SDL_INIT_VIDEO) < 0)
	{
		logmsg('e', "Couldn't init video.");
	}else{
		char *title = malloc(20 + strlen(inputfilename));
		sprintf(title, "FFT Visualisation: %s", inputfilename);
		window = SDL_CreateWindow(title, 
			SDL_WINDOWPOS_UNDEFINED, 
			SDL_WINDOWPOS_UNDEFINED, 
			SCREEN_WIDTH, SCREEN_HEIGHT, 
			SDL_WINDOW_SHOWN );

		if(window == NULL)
		{
			logmsg('e', "Couldn't create window.");
		}

		surface = SDL_GetWindowSurface(window);
		renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
	}
	TTF_Init();	
	cant = TTF_OpenFont(FONT);
}

void init_fft(void)
{
	/*
		Set up data buffers for FFT.
	*/

	//Input: FFT_WINSZ_IN number of double precision floating point values
	// (real numbers) representing amplitude against time.

	fft_in = (double *)fftw_malloc(sizeof(double) * FFT_WINSZ_IN);

	//Output: FFT_WINSZ_IN number of complex (Re,Im) samples representing
	// amplitude and phase against frequency.

	fft_out = (fftw_complex *)fftw_malloc(sizeof(fftw_complex) * FFT_WINSZ_IN);

	/*
		Plan: One dimensional (1d) real-to-complex (r2c) discrete 
		fourier transform (dft).

		See http://en.wikipedia.org/wiki/Discrete_Fourier_transform

		tl;dr Given a numeric waveform that is a sum of other waveforms
		(such as a musical piece with multiple instruments), find the
		wave frequencies that make up that original waveform (or the
		instruments that make up that piece).
	
		We're simply telling FFTW here that we want to use this particular
		transform. No math has been done yet.
	*/

	plan = fftw_plan_dft_r2c_1d(FFT_WINSZ_IN, fft_in, fft_out, FFTW_MEASURE); 

	if(plan == NULL)
	{
		logmsg('e', "couldn't setup FFT!");
	}
}

void setup_stream(void)
{
	/*
		Set up the stream buffer so we can pass data to FFTW
		and LibAO. This is done on a packet-by-packet (frame-by-frame)
		basis; each frame is ~10kByte (~192kbit + FF_I_B_P_S - overhead)
	*/
	av_init_packet(&p);
	f = av_frame_alloc();

	bufsz = 192000 +
		FF_INPUT_BUFFER_PADDING_SIZE;

	buffer = malloc(bufsz);
	
	p.data = buffer;
	p.size = bufsz;
}

void init_codec(void)
{

	/*
		Pull out the title/artist tags for use later
	*/

	dictionary = container->metadata;
	AVDictionaryEntry *songtitle = av_dict_get(dictionary, "title", NULL, 0);
	AVDictionaryEntry *songartist = av_dict_get(dictionary, "artist", NULL, 0);
	title = malloc(255);
	artist = malloc(255);
	strncpy(title, songtitle->value, 255);
	strncpy(artist, songartist->value, 255);

	/*
		Select the correct codec and codec context for this file.
		FFMPEG/LAVC usually does a decent job of this with no
		need for intervention.
	*/

	context = container->streams[activestreamindex]->codec;
	codec = avcodec_find_decoder(context->codec_id);

	if(!codec)
	{
		logmsg('e', "Couldn't locate an appropriate codec for this file.");
	}

	if(avcodec_open2(context, codec, &dictionary) < 0)
	{
		logmsg('e', "couldn't open codec.");
	}
	logmsg('n', "Codec initialised.");
}

void init_ao(void)
{
	/*
		Initialise libao and select the default
		driver. This can be selected by modifying ~/.libao
		or your OS's equivalent
	*/

	ao_initialize();
	driver = ao_default_driver_id();
	
	/*
		Select S16LE/44100 stereo output
		(signed, 16bit, little endian, 44.1kHz)
	*/
	memset(&aosampleformat, 0, sizeof(aosampleformat));
	aosampleformat.channels = 2;
	aosampleformat.rate = 44100;
	aosampleformat.byte_format = AO_FMT_LITTLE;
	aosampleformat.bits = 16;

	/*
		Open live (sound-card) output.
	*/
	dev = ao_open_live(driver, &aosampleformat, NULL);	

	if(dev == NULL)
	{
		logmsg('e', "libao error");
	}

	logmsg('n', "Successfully opened output, continuing...");
}

void init(void)
{
	logmsg('n', "Starting up...");

	/*
		ncurses and SDL screw with signals, define our own.
	*/
	signal(SIGINT, SIGINT_handler);

	/*
		Register codecs and allocate memory to hold
		information about our media.
	*/
	av_register_all();
	container = avformat_alloc_context();

	/*
		Open the file in LAVF.
	*/
	if(avformat_open_input(&container, inputfilename, NULL, NULL) < 0)
	{
		logmsg('e', "Couldn't open the file you requested.");
	}
	
	/*
		Mainly for debugging, this is all available
		in container->metadata anyway.
	*/

	logmsg('n', "File information:");
	av_dump_format(container, 0, inputfilename, 0);

	/*
		Ignore video streams.
	*/
	for(int i = 0; i < container->nb_streams; i++)
	{
		if(container->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO)
			activestreamindex = i; break;
	}

	if(activestreamindex < 0)
	{
		logmsg('e', "This file contains no audio.");
	}

	init_codec();
	init_ao();
	init_fft();
	setup_stream();
	init_SDL();
}

int main(int argc, char **argv)
{
	if(argc < 2)
	{
		logmsg('e', "Filename required.");
	}
	//todo: don't truncate
	strncpy(inputfilename, argv[1], 255);

	init();	

	play();

	fftw_destroy_plan(plan);
	fftw_free(fft_in);
	fftw_free(fft_out);

	SDL_Quit();

	ao_close(dev);
	ao_shutdown();

	//FIXME: how the hell do you shut down LAVC and close off the codec?
}
