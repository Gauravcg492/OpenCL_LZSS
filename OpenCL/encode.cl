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

encoded_string_t FindMatch(const unsigned int windowHead, unsigned int uncodedHead, unsigned int windowsize, unsigned char* slidingWindow, unsigned char* uncodedLookahead) {
  encoded_string_t matchData;
  unsigned int i;
  unsigned int j;

  matchData.length = 0;
  matchData.offset = 0;
  i = windowHead; /* start at the beginning of the sliding window */
  j = 0;

  while (1) {
    if (slidingWindow[i] == uncodedLookahead[uncodedHead]) 
    {
      /* we matched one. how many more match? */
      j = 1;

      while (slidingWindow[(i + j) % windowsize] == uncodedLookahead[((uncodedHead + j) % MAX_CODED)]) 
      {
        if (j >= MAX_CODED) {
          break;
        }
        j++;
      }

      if (j > matchData.length) {
        matchData.length = j;
        matchData.offset = i;
      }
    }

    if (j >= MAX_CODED) {
      matchData.length = MAX_CODED;
      break;
    }

    i = ((i + 1) % windowsize);
    if (i == windowHead) {
      /* we wrapped around */
      break;
    }
  }
  return matchData;
}

/*
 * Main encoding kernel
 */

__kernel void EncodeLZSS(__global struct FIFO *infifo, __global struct FIFO *outfifo, const unsigned int n,const unsigned int windowsize) 
{
    int id = get_global_id(0);
    int gid = get_group_id(0);
    int group_size = get_local_size(0);
    if (id < n * group_size) {
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
        
        /************************************************************************
        * Copy MAX_CODED bytes from the input file into the uncoded lookahead
        * buffer.
        ************************************************************************/
        //__local char out_array[BLOCKSIZE];
        // TODO try using local array for output and use all threads to copy it back to outfifo
        if (tidx == 0) {
            /* 8 code flags and encoded strings */
            unsigned char flags, flagPos, encodedData[16];
            int nextEncoded;                /* index into encodedData */
            encoded_string_t matchData;
            int c, read, len_out;
            unsigned int i;
            unsigned int len; /* length of string */
            /* head of sliding window and lookahead */
            unsigned int windowHead, uncodedHead;

            /* convert output file to bitfile */
            // bfpOut = MakeBitFile(fpOut, BF_WRITE);
            flags = 0;
            flagPos = 0x01;
            nextEncoded = 0;
            windowHead = 0;
            uncodedHead = 0;
            len_out = 0;
            read = 0
            for (len = 0; len < MAX_CODED && (c = infifo[gid].string[read]) != EOF; len++) {
                uncodedLookahead[len] = c;
                read++;
            }

            if (len != 0) {

                /* Look for matching string in sliding window */
                //i = InitializeSearchStructures();

                /*if (0 != i) {
                return i; /* InitializeSearchStructures returned an error *
                }*/

                matchData = FindMatch(windowHead, uncodedHead, windowsize, slidingWindow, uncodedLookahead);

                outfifo[gid].id = gid;
                /* now encoded the rest of the file until an EOF is read */
                while (len > 0) {
                    if (matchData.length > len) {
                        /* garbage beyond last data happened to extend match length */
                        matchData.length = len;
                    }

                    if (matchData.length <= MAX_UNCODED) {
                        /* not long enough match.  write uncoded flag and character */
                        //BitFilePutBit(UNCODED, bfpOut);
                        //outfifo[gid].string[len_out++] = 1;
                        //outfifo[gid].string[len_out++] = uncodedLookahead[uncodedHead];
                        //BitFilePutChar(uncodedLookahead[uncodedHead], bfpOut);
                        matchData.length = 1; /* set to 1 for 1 byte uncoded */
                        flags |= flagPos;       /* mark with uncoded byte flag */
                        encodedData[nextEncoded++] = uncodedLookahead[uncodedHead];
                    } else {
                        //unsigned int adjustedLen;

                        /* adjust the length of the match so minimun encoded len is 0*/
                        //adjustedLen = matchData.length - (MAX_UNCODED + 1);

                        /* match length > MAX_UNCODED.  Encode as offset and length. */
                        //BitFilePutBit(ENCODED, bfpOut);
                        //BitFilePutBitsNum(bfpOut, &matchData.offset, OFFSET_BITS,sizeof(unsigned int));
                        //BitFilePutBitsNum(bfpOut, &adjustedLen, LENGTH_BITS,sizeof(unsigned int));
                        //outfifo[gid].string[len_out++] = (unsigned char)matchData.length;
                        //outfifo[gid].string[len_out++] = (unsigned char)matchData.offset;
                        encodedData[nextEncoded++] = (unsigned char)((matchData.offset & 0x0FFF) >> 4);
                        encodedData[nextEncoded++] = (unsigned char)(((matchData.offset & 0x000F) << 4) |(matchData.length - (MAX_UNCODED + 1)));
                    }

                    if (flagPos == 0x80)
                    {
                        /* we have 8 code flags, write out flags and code buffer */
                        //putc(flags, outFile);
                        outfifo[gid].string[len_out++] = flags;
                        for (i = 0; i < nextEncoded; i++)
                        {
                            /* send at most 8 units of code together */
                            //putc(encodedData[i], outFile);
                            outfifo[gid].string[len_out++] = encodedData[i];
                        }
                        /* reset encoded data buffer */
                        flags = 0;
                        flagPos = 0x01;
                        nextEncoded = 0;
                    }
                    else
                    {
                        /* we don't have 8 code flags yet, use next bit for next flag */
                        flagPos <<= 1;
                    }

                    /********************************************************************
                    * Replace the matchData.length worth of bytes we've matched in the
                    * sliding window with new bytes from the input file.
                    ********************************************************************/
                    i = 0;
                    while ((i < matchData.length) && ((c = infifo[gid].string[read]) != EOF)) {
                        /* add old byte into sliding window and new into lookahead */
                        slidingWindow[windowHead] = uncodedLookahead[uncodedHead];
                        uncodedLookahead[uncodedHead] = c;
                        windowHead = (windowHead + 1) % windowsize;
                        uncodedHead = (uncodedHead + 1) % MAX_CODED;
                        i++;
                        read++;
                    }

                    /* handle case where we hit EOF before filling lookahead */
                    while (i < matchData.length) {
                        slidingWindow[windowHead] = uncodedLookahead[uncodedHead];
                        /* nothing to add to lookahead here */
                        windowHead = (windowHead + 1) % windowsize;
                        uncodedHead = (uncodedHead + 1) % MAX_CODED;
                        len--;
                        i++;
                    }

                    /* find match for the remaining characters */
                    matchData = FindMatch(windowHead, uncodedHead, windowsize, slidingWindow, uncodedLookahead);
                }

                /* write out any remaining encoded data */
                if (nextEncoded != 0)
                {
                    //putc(flags, outFile);
                    outfifo[gid].string[len_out++] = flags;
                    for (i = 0; i < nextEncoded; i++)
                    {
                        //putc(encodedData[i], outFile);
                        outfifo[gid].string[len_out++] = encodedData[i];
                    }
                }
                outfifo[gid].len = len_out;
            }
        }
    }
}