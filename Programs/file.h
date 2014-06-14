/*
 * BRLTTY - A background process providing access to the console screen (when in
 *          text mode) for a blind person using a refreshable braille display.
 *
 * Copyright (C) 1995-2014 by The BRLTTY Developers.
 *
 * BRLTTY comes with ABSOLUTELY NO WARRANTY.
 *
 * This is free software, placed under the terms of the
 * GNU General Public License, as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any
 * later version. Please see the file LICENSE-GPL for details.
 *
 * Web Page: http://mielke.cc/brltty/
 *
 * This software is maintained by Dave Mielke <dave@mielke.cc>.
 */

#ifndef BRLTTY_INCLUDED_FILE
#define BRLTTY_INCLUDED_FILE

#include <stdio.h>
#include <stdarg.h>

#include "get_sockets.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

extern int isPathDelimiter (const char character);
extern int isAbsolutePath (const char *path);
extern char *getPathDirectory (const char *path);
extern const char *locatePathName (const char *path);
extern const char *locatePathExtension (const char *path);
extern int isExplicitPath (const char *path);

extern char *joinPath (const char *const *components, unsigned int count);
extern char *makePath (const char *directory, const char *file);

extern int hasFileExtension (const char *path, const char *extension);
extern char *replaceFileExtension (const char *path, const char *extension);
extern char *ensureFileExtension (const char *path, const char *extension);
extern char *makeFilePath (const char *directory, const char *name, const char *extension);

extern int testPath (const char *path);
extern int testFilePath (const char *path);
extern int testProgramPath (const char *path);
extern int testDirectoryPath (const char *path);

extern int createDirectory (const char *path);
extern int ensureDirectory (const char *path);

extern const char *writableDirectory;
extern const char *getWritableDirectory (void);
extern char *makeWritablePath (const char *file);

extern char *getWorkingDirectory (void);
extern int setWorkingDirectory (const char *path);

extern char *getHomeDirectory (void);
extern const char *const *getAllOverrideDirectories (void);
extern const char *getPrimaryOverrideDirectory (void);

extern int acquireFileLock (int file, int exclusive);
extern int attemptFileLock (int file, int exclusive);
extern int releaseFileLock (int file);

extern void registerProgramStream (const char *name, FILE **stream);
extern void flushStream (FILE *stream);

#if defined(_POSIX_THREAD_SAFE_FUNCTIONS) && !defined(__MINGW32__)
#define lockStream(stream) flockfile((stream))
#define unlockStream(stream) funlockfile((stream))
#else /* _POSIX_THREAD_SAFE_FUNCTIONS */
#define lockStream(stream)
#define unlockStream(stream)
#endif /* _POSIX_THREAD_SAFE_FUNCTIONS */

extern FILE *openFile (const char *path, const char *mode, int optional);

typedef int LineHandler (char *line, void *data);
extern int processLines (FILE *file, LineHandler handleLine, void *data);
extern int readLine (FILE *file, char **buffer, size_t *size);

extern size_t formatInputError (char *buffer, size_t size, const char *file, const int *line, const char *format, va_list argp);

extern ssize_t readFileDescriptor (FileDescriptor fileDescriptor, void *buffer, size_t size);
extern ssize_t writeFileDescriptor (FileDescriptor fileDescriptor, const void *buffer, size_t size);

#ifdef GOT_SOCKETS
extern ssize_t readSocketDescriptor (SocketDescriptor socketDescriptor, void *buffer, size_t size);
extern ssize_t writeSocketDescriptor (SocketDescriptor socketDescriptor, const void *buffer, size_t size);
#endif /* GOT_SOCKETS */

extern const char *getNamedPipeDirectory (void);
extern int createAnonymousPipe (FileDescriptor *pipeInput, FileDescriptor *pipeOutput);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* BRLTTY_INCLUDED_FILE */
