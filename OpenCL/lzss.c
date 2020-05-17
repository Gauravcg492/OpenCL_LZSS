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
                free(infifo);
                free(outfifo);
                exit(3);
            }
        }
        infifo[i].len = result;
    }
    callKernel(infifo, outfifo, no_of_blocks, "encode.cl", "EncodeLZSS");

    // write to file
    fputc((char)no_of_blocks, fpout);
    for(int i=0; i<no_of_blocks; i++)
    {
        fwrite(outfifo[i].string, 1, outfifo[i].len, fpout);
        fputc(0x1D, fpout);
    }

    // free memory
    free(infifo);
    free(outfifo);
}

int DecodeLZSS(FILE *fpIn, FILE *fpOut)
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

    // get the total no of blocks used from the first character of the compressed string
    int no_of_blocks = (int) fgetc(fpIn);

    infifo = (FIFO *)malloc(sizeof(FIFO) * no_of_blocks);
    outfifo = (FIFO *)malloc(sizeof(FIFO) * no_of_blocks);

    int block_no = 0;
    int len_str = 0;
    int c;
    while((c = fgetc(fpIn)) == EOF)
    {
        if( c == 0x1D)
        {
            infifo[block_no].id = block_no;
            infifo[block_no].len = len_str;
            block_no++;
            len_str = 0;
        } else
        {
            infifo[block_no].string[len_str++] = c;
        }        
    }
    if(block_no != no_of_blocks)
    {
        printf("Some error occurred during Compression\n");
        free(infifo);
        free(outfifo);
        exit(1);
    }
    callKernel(infifo, outfifo, no_of_blocks, "decode.cl", "DecodeLZSS");

    // write to file
    for(int i=0; i<no_of_blocks; i++)
    {
        fwrite(outfifo[i].string, 1, outfifo[i].len, fpout);
    }
    // free memory
    free(infifo);
    free(outfifo);
}

void callKernel(FIFO *infifo, FIFO *outfifo, int no_of_blocks, char* cl_filename, char* cl_kernelname)
{
    FILE *fp;
    char *source_str;
    size_t source_size;

    fp = fopen(cl_filename, "r");
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

    kernel = clCreateKernel(program, cl_kernelname, &err);

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

