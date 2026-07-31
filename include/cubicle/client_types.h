#ifndef CUBICLE_CLIENT_TYPES_H
#define CUBICLE_CLIENT_TYPES_H

/* Compatibility aggregate. New code should include the specific headers. */
#include "cubicle/types.h"
#include "cubicle/process.h"
#include "cubicle/workspace.h"

#define CUBICLE_MODE_STREAM CUBICLE_PROCESS_STREAM
#define CUBICLE_MODE_TTY CUBICLE_PROCESS_TTY
#define CUBICLE_MODE_TTY_CAPTURED_STDERR CUBICLE_PROCESS_TTY_CAPTURED_STDERR

#endif
