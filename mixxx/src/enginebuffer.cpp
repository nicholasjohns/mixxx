/***************************************************************************
                          enginebuffer.cpp  -  description
                             -------------------
    begin                : Wed Feb 20 2002
    copyright            : (C) 2002 by Tue and Ken Haste Andersen
    email                : 
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "enginebuffer.h"

EngineBuffer::EngineBuffer(DlgPlaycontrol *playcontrol, DlgChannel *channel, MidiObject *midi, const char *filename)
{
  PlayButton = new ControlPushButton("playbutton", simulated_latching, PORT_B, 0, midi);
  PlayButton->setValue(on);
  connect(playcontrol->PushButtonPlay, SIGNAL(pressed()), PlayButton, SLOT(pressed()));
  connect(playcontrol->PushButtonPlay, SIGNAL(released()), PlayButton, SLOT(released()));
  connect(PlayButton, SIGNAL(valueChanged(valueType)), this, SLOT(slotUpdatePlay(valueType)));

  rateSlider = new ControlPotmeter("rateslider", ADC3, midi, 0.9,1.1);
  rateSlider->slotSetPosition(64);
  rate = rateSlider->getValue();
  connect(channel->SliderRate, SIGNAL(valueChanged(int)), rateSlider, SLOT(slotSetPosition(int)));
  connect(rateSlider, SIGNAL(valueChanged(FLOAT)), this, SLOT(slotUpdateRate(FLOAT)));
  connect(rateSlider, SIGNAL(recievedMidi(int)), channel->SliderRate, SLOT(setValue(int)));

  wheel = new ControlRotary("wheel", PORT_D, midi);
  connect(playcontrol->DialPlaycontrol, SIGNAL(valueChanged(int)), wheel, SLOT(slotSetPosition(int)));
  connect(wheel, SIGNAL(valueChanged(FLOAT)), this, SLOT(slotUpdateRate(FLOAT)));
  connect(wheel, SIGNAL(recievedMidi(int)), playcontrol->DialPlaycontrol, SLOT(setValue(int)));

  connect(this, SIGNAL(position(int)), channel->LCDposition, SLOT(display(int)));
  //connect(this, SIGNAL(position(int)), channel->SliderPosition, SLOT(setValue(int)));

  connect(channel->SliderPosition, SIGNAL(valueChanged(int)), this, SLOT(slotPosition(int)));
  // Allocate temporary buffer
  read_buffer_size = READBUFFERSIZE;
  chunk_size = READCHUNKSIZE;
  temp = new SAMPLE[2*chunk_size]; // Temporary buffer for the raw samples
  // note that the temp buffer is made extra large.
  readbuffer = new CSAMPLE[read_buffer_size];

  // Allocate semaphore
  buffers_read_ahead = new sem_t;
  sem_init(buffers_read_ahead, 0, 1);

  // Semaphore for stopping thread
  requestStop = new QSemaphore(1);

  // Open the track:
  file = 0;
  newtrack(filename);

  buffer = new CSAMPLE[MAX_BUFFER_LEN];
}

EngineBuffer::~EngineBuffer(){
  qDebug("dealloc buffer");
  if (running())
  {
    qDebug("Stopping buffer");
    stop();
  }
  qDebug("buffer waiting...");

  qDebug("buffer actual dealloc");
  if (file != 0) delete file;
  delete [] temp;
  delete [] readbuffer;
  delete buffers_read_ahead;
  delete PlayButton;
  delete wheel;
  delete rateSlider;
  delete buffer;
}

void EngineBuffer::newtrack(const char* filename) {
  // If we are already playing a file, then get rid of it:
  if (file != 0) delete file;
  /*
    Open the file:
  */
  int i=strlen(filename)-1;
  while ((filename[i] != '.') && (i>0))
    i--;
  if (i == 0) {
    qFatal("Wrong filename: %s.",filename);
    std::exit(-1);
  }
  char ending[80];
  strcpy(ending,&filename[i]);
  if (!strcmp(ending,".wav"))
    file = new AFlibfile(filename);
  else if (!strcmp(ending,".mp3") || (!strcmp(ending,".MP3")))
	file = new SoundSourceHeavymp3(filename);

  if (file==0) {
    qFatal("Error opening %s", filename);
    std::exit(-1);
  }
  // Initialize position in read buffer:
  filepos = 0;
  frontpos = 0;
  play_pos = 0;
  direction = 1;
  // ...and read one chunk to get started:
  getchunk();
}

void EngineBuffer::start() {
    qDebug("starting EngineBuffer...");
    QThread::start();
    qDebug("started!");
}

void EngineBuffer::stop()
{
  sem_post(buffers_read_ahead);
  requestStop->operator++(1);
  wait();
  requestStop->operator--(1);
}

void EngineBuffer::run() {
  while(requestStop->available()) {
    // Wait for playback if in buffer is filled.
    sem_wait(buffers_read_ahead);
    // Check if the semaphore is too large:
    int sem_value;
    sem_getvalue(buffers_read_ahead, &sem_value);
    if (sem_value != 0)
      qDebug("Reader is requesting %d reads at once.", sem_value+1);
    else
      // Read a new chunk:
      getchunk();
  }
};
/*
  Called when the playbutten is pressed
*/
void EngineBuffer::slotUpdatePlay(valueType) {
  static int start_seek;
  if (PlayButton->getPosition()==down) {
    qDebug("Entered seeking mode");
    rate = 0;
    start_seek = wheel->getPosition();
  }
  else if (PlayButton->getPosition()==up) {
    int end_seek = wheel->getPosition();
    if (abs(start_seek - end_seek) > 2) {
      // A seek has occured. Find new filepos:
      if ((wheel->direction==1) &&(end_seek < start_seek))
	end_seek += 128;
      else
	if ((wheel->direction==-1) && (end_seek > start_seek))
	  end_seek -= 128;
      //cout << "Seeking " << (FLOAT)((end_seek-start_seek)/128.) 
      //   << ".";
      seek((FLOAT)(end_seek-start_seek)/128);
    }
    qDebug("Ended seeking");
  }
  slotUpdateRate(rateSlider->getValue());
}

void EngineBuffer::slotUpdateRate(FLOAT) {
  if (PlayButton->getValue()==on)
      rate = rateSlider->getValue() + 4*wheel->getValue();
  else
      if (PlayButton->getPosition()==down)
	  rate = 0;
      else
	  rate = 4*wheel->getValue();
  //qDebug("Rate value: %f",rate);
}
/*
  Read a new chunk into the readbuffer:
*/
void EngineBuffer::getchunk() {
  // Save direction if player changes it's mind while we're processing.
  //int saved_direction = direction;

  // update frontpos so that we wont be called while reading:
  frontpos = (frontpos+chunk_size)%read_buffer_size;
  qDebug("Reading...");
  // For a read backwards, we have to change the position in the file:
  /*if (direction == -1) {
    filepos -= (read_buffer_size + chunk_size);
    file->seek(filepos);
    //afSeekFrame(fh, AF_DEFAULT_TRACK, (AFframecount) (filepos/channels));
  } else*/

  // Read a chunk
  unsigned samples_read = file->read(chunk_size, temp);

  if (samples_read != chunk_size)
      qDebug("Didn't get as many samples as we asked for: %d:%d", chunk_size, samples_read);

  // Convert from SAMPLE to CSAMPLE. Should possibly be optimized
  // using assembler code from music-dsp archive.
  filepos += samples_read;
  unsigned new_frontpos = (frontpos-chunk_size+read_buffer_size)%read_buffer_size;
  //qDebug("Reading into position %d %f",new_frontpos, play_pos);
  for (unsigned j=0; j<samples_read; j++) {
    readbuffer[new_frontpos] = temp[j];
    new_frontpos ++;
    if (new_frontpos > read_buffer_size) new_frontpos = 0;
  }
  qDebug("Done reading.");
}
/*
  This is called when the positionslider is released:
*/
void EngineBuffer::slotPosition(int newvalue) {
  seek((FLOAT)newvalue/102 - play_pos/(FLOAT)file->length());
}
/*
  Moves the playpos forward change%
*/
void EngineBuffer::seek(FLOAT change) {
  double new_play_pos = play_pos + change*file->length();
  if (new_play_pos > file->length()) new_play_pos = file->length();
  if (new_play_pos < 0) new_play_pos = 0;
  filepos = (long unsigned)new_play_pos;
  qDebug("Seeking %g to %g",change, new_play_pos/file->length());
  file->seek(filepos);
  frontpos = chunk_size*(((int)floor(play_pos/chunk_size))%2);
  qDebug("%li",frontpos);
  getchunk();
  getchunk();
  qDebug("done seeking.");
  play_pos = new_play_pos;
}

bool even(long n) {
  if ((n/2) != (n+1)/2)
    return false;
  else
    return true;
}

// -------- ------------------------------------------------------
// Purpose: Make a check if it is time to start reading some
//          more samples. If it is, update the semaphore.
// Input:   -
// Output:  -
// -------- ------------------------------------------------------
void EngineBuffer::checkread() {
  static int sem_value; // place to store the value of the semaphore for read
  static int pending_time = 0;

  bool send_request = false;

  if ((distance((long)floor(play_pos)%read_buffer_size, frontpos)
      < READAHEAD) && (filepos != (unsigned long) file->length())) {
    direction = 1;
    send_request = true;
  } else
    if ((distance(frontpos, (long)floor(play_pos)%read_buffer_size)
	< READAHEAD) && (filepos > read_buffer_size)){
      direction = -1;
      send_request = true;
    }

  if (send_request) {
    // check if we still have a request pending:
    //std::cout << (long)play_pos << ", " << frontpos << ", " << filepos << "\n";
    sem_getvalue(buffers_read_ahead, &sem_value);
    if (sem_value == 0) {
	  //qDebug("Frontpos: %i Playpos: %i),frontpos,play_pos);
      //std::cout << frontpos << "," <<play_pos<<","<<(long)floor(play_pos)%read_buffer_size<<"\n";
      //cout << filepos << "," << filelength << "\n";
      sem_post(buffers_read_ahead);
      pending_time = 0;
    }
    else {
      pending_time ++;
      //std::cout << pending_time << "\n";
      if (pending_time == 0.9*READAHEAD/BUFFER_SIZE)
	qDebug("Warning: reader is (close to) lacking behind player!");
    }
  }
}

/*
  Helper function which returns the distance in the readbuffer between
  _start and end.
*/
long EngineBuffer::distance(const long _start, const long end) {
  long start = _start;
  if (start > end)
    start -= read_buffer_size;
  return end-start;
}

void EngineBuffer::writepos() {
  static FLOAT lastwrite = 0.;
  FLOAT newwrite = play_pos/file->length();
  if (floor(fabs(newwrite-lastwrite)*100) >= 1) {
      emit position((int)(100*newwrite));
      lastwrite = newwrite;
  }
}

 FLOAT EngineBuffer::min(const FLOAT a, const FLOAT b) {
  if (a > b)
    return b;
  else
    return a;
}

FLOAT EngineBuffer::max(const FLOAT a, const FLOAT b) {
  if (a > b)
    return a;
  else
    return b;
}

CSAMPLE *EngineBuffer::process(CSAMPLE *, int buf_size) {
  long prev;
  for (int i=0; i<buf_size; i+=2) {
    prev = (long)floor(play_pos)%read_buffer_size;
    if (!even(prev)) prev--;
    long next = (prev+2)%read_buffer_size;
    FLOAT frac = play_pos - floor(play_pos);
    buffer[i  ] = readbuffer[prev  ] +frac*(readbuffer[next  ]-readbuffer[prev  ]);
    buffer[i+1] = readbuffer[prev+1] +frac*(readbuffer[next+1]-readbuffer[prev+1]);
    play_pos += 2.*rate;
    play_pos = max(0.,min(file->length(), play_pos));
  }

  checkread();
  // Check the wheel:
  wheel->updatecounter(buf_size);
  // Write position to the gui: 
  writepos();

  return buffer;
}


