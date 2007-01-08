/*
 * BRLTTY - A background process providing access to the console screen (when in
 *          text mode) for a blind person using a refreshable braille display.
 *
 * Copyright (C) 1995-2007 by The BRLTTY Developers.
 *
 * BRLTTY comes with ABSOLUTELY NO WARRANTY.
 *
 * This is free software, placed under the terms of the
 * GNU General Public License, as published by the Free Software
 * Foundation.  Please see the file COPYING for details.
 *
 * Web Page: http://mielke.cc/brltty/
 *
 * This software is maintained by Dave Mielke <dave@mielke.cc>.
 */

/* Alva/braille.h - Configurable definitions for the Alva driver
 * Copyright (C) 1995-1998 by Nicolas Pitre <nico@cam.org>
 *
 */

/* used by speech.c */
extern int AL_writeData( unsigned char *data, int len );

/* Known Device Identification Numbers (not to be changed) */
#define ABT_AUTO	        0XFF	/* for new firmware only */
#define ABT320		0X00	/* ABT 320 */
#define ABT340		0X01	/* ABT 340 */
#define ABT34D		0X02	/* ABT 340 Desktop */
#define ABT380		0X03	/* ABT 380 */
#define ABT382		0X04	/* ABT 382 Twin Space */
#define DEL420		0X0A	/* Delphi 20 */
#define DEL440		0X0B	/* Delphi 40 */
#define DEL44D		0X0C	/* Delphi 40 Desktop */
#define DEL480		0X0D 	/* Delphi Multimedia */
#define SAT544		0X0E	/* Satellite 544 */
#define SAT570P		0X0F 	/* Satellite 570 Pro */
#define SAT584P		0X10 	/* Satellite 584 Pro */
#define SAT544T		0X11 	/* Satellite 544 Traveller */


/***** User Settings *****  Edit as necessary for your system. */


/* Define next line to 1 if you have a firmware older than version 010495 */
#define ABT3_OLD_FIRMWARE 0


/* Specify the terminal model that you'll be using.
 * Use one of the previously defined device ID.
 * WARNING: Do not use BRL_AUTO if you have ABT3_OLD_FIRMWARE defined to 1 !
 */
#define MODEL   ABT_AUTO


#define BAUDRATE 9600


/* Delay in miliseconds between forced full refresh of the display.
 * This is to minimize garbage effects due to noise on the serial line.
 */
#define REWRITE_INTERVAL 1000
