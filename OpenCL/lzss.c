#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <CL/opencl.h>
#include "bitfile.h"
#define WINDOWSIZE 4096
#define BLOCKSIZE 1048576
#define MINSIZE 1048576
#define MAX_SOURCE_SIZE (0x100000)

typedef struct FIFO
{
    int id;
    int len;
    char string[BLOCKSIZE];
} FIFO;

int EncodeLZSS(FILE *fpin, FILE *fpout)
{
    if(fpin == NULL || fpout == NULL)
    {
        printf("No file\n");
        exit(1);
    }
    FIFO *infifo;
    FIFO *outfifo;
    fseek(fpin, 0, SEEK_END);
    long totalSize = ftell(fpin);
    fseek(fpin, 0, SEEK_SET);
    if (totalSize < MINSIZE)
    {
        printf("No use of parallel GPU computation");
        return 0;
    }
    int bsize = BLOCKSIZE;
    int no_of_blocks = ceil(totalSize / bsize);
    int padding = totalSize % bsize;
    padding = (padding) ? (bsize - padding) : 0;

    infifo = (FIFO *)malloc(sizeof(FIFO) * no_of_blocks);
    outfifo = (FIFO *)malloc(sizeof(FIFO) * no_of_blocks);
    for (int i = 0; i < no_of_blocks; i++)
    {
        infifo[i].id = i;
        int result = fread(infifo[i].string, 1, bsize, fpin);
        if (result != bsize)
        {
            if (i != no_of_blocks - 1)
            {
                printf("Reading error1, expected size %d, read size %d ", bsize, result);
                exit(3);
            }
        }
        infifo[i].len = result;
    }
    callKernel(infifo, outfifo, no_of_blocks);

    // write to file
    for(int i=0; i<no_of_blocks; i++)
    {
        fwrite(outfifo[i].string, 1, outfifo[i].len, fpout);
    }

    // free memory
    free(infifo);
    free(outfifo);
}

void callKernel(FIFO *infifo, FIFO *outfifo, int no_of_blocks)
{
    FILE *fp;
    char *source_str;
    size_t source_size;

    fp = fopen("encode.cl", "r");
    if (!fp)
    {
        fprintf(stderr, "Failed to load kernel.\n");
        exit(1);
    }
    source_str = (char *)malloc(MAX_SOURCE_SIZE);
    source_size = fread(source_str, 1, MAX_SOURCE_SIZE, fp);
    fclose(fp);

    unsigned int window_size = WINDOWSIZE;

    //device structs
    cl_mem d_inf;
    cl_mem d_outf;

    // opencl variables
    cl_platform_id cpPlatform;
    cl_device_id dev_id;
    cl_context context;
    cl_command_queue queue;
    cl_program program;
    cl_kernel kernel;
    cl_int err;

    size_t bytes = sizeof(FIFO) * no_of_blocks;
    size_t globalSize;
    size_t localSize = 128;
    globalSize = no_of_blocks * localSize;
    //globalSize = ceil(no_of_blocks / (float)localSize) * localSize;

    err = clGetPlatformIDs(1, &cpPlatform, NULL);

    err = clGetDeviceIDs(cpPlatform, CL_DEVICE_TYPE_GPU, 1, &dev_id, NULL);

    context = clCreateContext(0, 1, &dev_id, NULL, NULL, &err);

    queue = clCreateCommandQueueWithProperties(context, dev_id, NULL, &err);

    program = clCreateProgramWithSource(context, 1, (const char**)&source_str, (const size_t*)&source_size, &err);

    clBuildProgram(program, 0, NULL, NULL, NULL, NULL);

    kernel = clCreateKernel(program, "EncodeLZSS", &err);

    d_inf = clCreateBuffer(context, CL_MEM_READ_ONLY, bytes, NULL, NULL);
    d_outf = clCreateBuffer(context, CL_MEM_READ_ONLY, bytes, NULL, NULL);

    err = clEnqueueWriteBuffer(queue, d_inf, CL_TRUE, 0, bytes, infifo, 0, NULL, NULL);
    //err |= clEnqueueWriteBuffer(queue, d_outf, CL_TRUE, 0, bytes, outfifo, 0, NULL, NULL);

    err = clSetKernelArg(kernel, 0, sizeof(cl_mem), &d_inf);
    err |= clSetKernelArg(kernel, 1, sizeof(cl_mem), &d_outf);
    err |= clSetKernelArg(kernel, 2, sizeof(unsigned int), &no_of_blocks);
    err |= clSetKernelArg(kernel, 3, sizeof(unsigned int), &window_size);

    err = clEnqueueNDRangeKernel(queue, kernel, 1, NULL, &globalSize, &localSize, 0, NULL, NULL);

    clFinish(queue);

    clEnqueueReadBuffer(queue, d_outf, CL_TRUE, 0, bytes, outfifo, 0, NULL, NULL);

    clReleaseMemObject(d_inf);
    clReleaseMemObject(d_outf);
    clReleaseProgram(program);
    clReleaseKernel(kernel);
    clReleaseCommandQueue(queue);
    clReleaseContext(context);
}

int DecodeLZSS(FILE *fpIn, FILE *fpOut)
{
    bit_file_t *bfpIn;
    int c;
    unsigned int i, nextChar;
    encoded_string_t code; /* offset/length code for string */

    /* use stdin if no input file */
    if ((NULL == fpIn) || (NULL == fpOut))
    {
        errno = ENOENT;
        return -1;
    }

    /* convert input file to bitfile */
    bfpIn = MakeBitFile(fpIn, BF_READ);

    if (NULL == bfpIn)
    {
        perror("Making Input File a BitFile");
        return -1;
    }

    /************************************************************************
    * Fill the sliding window buffer with some known vales.  EncodeLZSS must
    * use the same values.  If common characters are used, there's an
    * increased chance of matching to the earlier strings.
    ************************************************************************/
    memset(slidingWindow, ' ', WINDOW_SIZE * sizeof(unsigned char));

    nextChar = 0;

    while (1)
    {
        if ((c = BitFileGetBit(bfpIn)) == EOF)
        {
            /* we hit the EOF */
            break;
        }

        if (c == UNCODED)
        {
            /* uncoded character */
            if ((c = BitFileGetChar(bfpIn)) == EOF)
            {
                break;
            }

            /* write out byte and put it in sliding window */
            putc(c, fpOut);
            slidingWindow[nextChar] = c;
            nextChar = Wrap((nextChar + 1), WINDOW_SIZE);
        }
        else
        {
            /* offset and length */
            code.offset = 0;
            code.length = 0;

            if ((BitFileGetBitsNum(bfpIn, &code.offset, OFFSET_BITS,
                                   sizeof(unsigned int))) == EOF)
            {
                break;
            }

            if ((BitFileGetBitsNum(bfpIn, &code.length, LENGTH_BITS,
                                   sizeof(unsigned int))) == EOF)
            {
                break;
            }

            code.length += MAX_UNCODED + 1;

            /****************************************************************
            * Write out decoded string to file and lookahead.  It would be
            * nice to write to the sliding window instead of the lookahead,
            * but we could end up overwriting the matching string with the
            * new string if abs(offset - next char) < match length.
            ****************************************************************/
            for (i = 0; i < code.length; i++)
            {
                c = slidingWindow[Wrap((code.offset + i), WINDOW_SIZE)];
                putc(c, fpOut);
                uncodedLookahead[i] = c;
            }

            /* write out decoded string to sliding window */
            for (i = 0; i < code.length; i++)
            {
                slidingWindow[Wrap((nextChar + i), WINDOW_SIZE)] =
                    uncodedLookahead[i];
            }

            nextChar = Wrap((nextChar + code.length), WINDOW_SIZE);
        }
    }

    /* we've decoded everything, free bitfile structure */
    BitFileToFILE(bfpIn);

    return 0;
}
