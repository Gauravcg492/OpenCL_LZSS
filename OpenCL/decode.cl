#define MAX_UNCODED 2
#define MAX_CODED ((1 << 4) + MAX_UNCODED)
#define BLOCKSIZE 1048576

typedef struct encoded_string_t {
  unsigned int offset; /* offset to start of longest match */
  unsigned int length; /* length of longest match */
} encoded_string_t;

struct __attribute__((packed)) FIFO {
  int id;
  int len;
  char string[BLOCKSIZE];
};

__kernel void DecodeLZSS(__global struct FIFO *infifo, __global struct FIFO *outfifo, const unsigned int n, const unsigned int windowsize)
{
    int id = get_global_id(0);
    int gid = get_group_id(0);
    int group_size = get_local_size(0);

    if(id < n * group_size) 
    {
        /* cyclic buffer sliding window of already read characters */
        __local unsigned char slidingWindow[windowsize];
        __local unsigned char uncodedLookahead[MAX_CODED];

        /************************************************************************
        * Fill the sliding window buffer with some known vales.  DecodeLZSS must
        * use the same values.  If common characters are used, there's an
        * increased chance of matching to the earlier strings.
        ************************************************************************/
        // memset(slidingWindow, ' ', WINDOW_SIZE * sizeof(unsigned char));
        int tidx = get_local_id(0);
        #pragma unroll
        for (int t = tidx; t < windowsize; t += group_size) {
            slidingWindow[t] = ' ';
        }
        barrier(CLK_LOCAL_MEM_FENCE);


        if(tidx == 0)
        {
            int c, read, len_out;
            unsigned char flags, flagsUsed;     /* encoded/not encoded flag */
            unsigned int i, nextChar;
            encoded_string_t code; /* offset/length code for string */

            /* initialize variables */
            flags = 0;
            flagsUsed = 7;
            nextChar = 0;
            read = 0;
            len_out = 0;

            outfifo[gid].id = gid;
            while (1)
            {
                flags >>= 1;
                flagsUsed++;

                if (flagsUsed == 8)
                {
                    /* shifted out all the flag bits, read a new flag */
                    //if ((c = getc(inFile)) == EOF)
                    if ((c = infifo[gid].string[read]) == EOF)
                    {
                        break;
                    }
                    read++;
                    flags = c & 0xFF;
                    flagsUsed = 0;
                }

                if (flags & 0x01)
                {
                    /* uncoded character */
                    //if ((c = BitFileGetChar(bfpIn)) == EOF)
                    if ((c = infifo[gid].string[read]) == EOF)
                    {
                        break;
                    }
                    read++;
                    /* write out byte and put it in sliding window */
                    //putc(c, fpOut);
                    outfifo[gid].string[len_out++] = c;
                    slidingWindow[nextChar] = c;
                    nextChar = (nextChar + 1) % windowsize;
                }
                else
                {
                    /* offset and length */
                    code.offset = 0;
                    code.length = 0;

                    /* offset and length */
                    if ((code.offset = infifo[gid].string[read]) == EOF)
                    {
                        break;
                    }
                    read++;
                    if ((code.length = infifo[gid].string[read]) == EOF)
                    {
                        break;
                    }
                    read++;

                    /* unpack offset and length */
                    //code.length += MAX_UNCODED + 1;
                    code.offset <<= 4;
                    code.offset |= ((code.length & 0x00F0) >> 4);
                    code.length = (code.length & 0x000F) + MAX_UNCODED + 1;

                    /****************************************************************
                    * Write out decoded string to file and lookahead.  It would be
                    * nice to write to the sliding window instead of the lookahead,
                    * but we could end up overwriting the matching string with the
                    * new string if abs(offset - next char) < match length.
                    ****************************************************************/
                    for (i = 0; i < code.length; i++)
                    {
                        c = slidingWindow[(code.offset + i) % windowsize];
                        //putc(c, fpOut);
                        outfifo[gid].string[len_out++] = c;
                        uncodedLookahead[i] = c;
                    }

                    /* write out decoded string to sliding window */
                    for (i = 0; i < code.length; i++)
                    {
                        slidingWindow[(nextChar + i) % windowsize] = uncodedLookahead[i];
                    }

                    nextChar = (nextChar + code.length) % windowsize;
                }
            }
            outfifo[gid].len = len_out;
        }
    }
}