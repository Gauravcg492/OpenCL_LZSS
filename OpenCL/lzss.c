#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <CL/opencl.h>
#define WINDOWSIZE 4096
#define BLOCKSIZE 1048576
#define MINSIZE 1048576
#define MAX_UNCODED 2
#define MAX_CODED ((1 << 4) + MAX_UNCODED)
#define MAX_SOURCE_SIZE (0x100000)

typedef struct FIFO
{
    int id;
    int len;
    char string[BLOCKSIZE];
} FIFO;

void callKernel(FIFO *infifo, FIFO *outfifo, int no_of_blocks, char* cl_filename, char* cl_kernelname);

int EncodeLZSS(FILE *fpIn, FILE *fpOut)
{
    setbuf(stdout,NULL);
    if(fpIn == NULL || fpOut == NULL)
    {
        printf("No file\n");
        exit(1);
    }
    FIFO *infifo;
    FIFO *outfifo;
    fseek(fpIn, 0, SEEK_END);
    long totalSize = ftell(fpIn);
    fseek(fpIn, 0, SEEK_SET);
    if (totalSize < MINSIZE)
    {
        printf("No use of parallel GPU computation");
        return 0;
    }
    int bsize = BLOCKSIZE;
    int no_of_blocks = ceil((float)totalSize /(float)bsize);
    int padding = totalSize % bsize;
    padding = (padding) ? (bsize - padding) : 0;
    printf("Num of blocks %d\nBuffer size %d\nTotal Size %ld\n", no_of_blocks, bsize, totalSize);

    infifo = (FIFO *)malloc(sizeof(FIFO) * no_of_blocks);
    outfifo = (FIFO *)malloc(sizeof(FIFO) * no_of_blocks);
    printf("Reading file\n");
    for (int i = 0; i < no_of_blocks; i++)
    {
        infifo[i].id = i;
        int result = fread(infifo[i].string, 1, bsize, fpIn);
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
    for(int i=0; i<no_of_blocks; i++)
    {
        printf("%d ", infifo[i].len);
    }
    printf("\nCalling Kernel\n");
    callKernel(infifo, outfifo, no_of_blocks, "encode.cl", "EncodeLZSS");
    printf("Kernel Completed\n");

    // write to file
    fputc((char)no_of_blocks, fpOut);
    for(int i=0; i<no_of_blocks; i++)
    {
        printf("%d ", outfifo[i].len);
        fwrite(outfifo[i].string, 1, outfifo[i].len, fpOut);
        fputc(0x1D, fpOut);
    }
    printf("\nWrite to file completed\n");
    // free memory
    free(infifo);
    free(outfifo);
}

int DecodeLZSS(FILE *fpIn, FILE *fpOut)
{
    if(fpIn == NULL || fpOut == NULL)
    {
        printf("No file\n");
        exit(1);
    }
    setbuf(stdout,NULL);
    FIFO *infifo;
    FIFO *outfifo;
    fseek(fpIn, 0, SEEK_END);
    long totalSize = ftell(fpIn);
    fseek(fpIn, 0, SEEK_SET);

    // get the total no of blocks used from the first character of the compressed string
    int no_of_blocks = (int) fgetc(fpIn);
    printf("No of blocks %d\n", no_of_blocks);
    infifo = (FIFO *)malloc(sizeof(FIFO) * no_of_blocks);
    outfifo = (FIFO *)malloc(sizeof(FIFO) * no_of_blocks);

    int block_no = 0;
    int len_str = 0;
    int c;
    printf("Reading file\n");
    while((c = fgetc(fpIn)) == EOF)
    {
        if( c == 0x1D)
        {
            infifo[block_no].id = block_no;
            infifo[block_no].len = len_str;
            printf("%d ", infifo[block_no].len);
            block_no++;
            len_str = 0;            
        } else
        {
            infifo[block_no].string[len_str++] = c;
        }        
    }
    printf("\nfile read completed\n");
    if(block_no != no_of_blocks)
    {
        printf("Some error occurred during Compression\n");
        free(infifo);
        free(outfifo);
        exit(1);
    }
    printf("Calling kernel\n");
    callKernel(infifo, outfifo, no_of_blocks, "decode.cl", "DecodeLZSS");
    printf("Kernel completed\n");
    // write to file
    for(int i=0; i<no_of_blocks; i++)
    {
        printf("%d ", outfifo[i].len);
        fwrite(outfifo[i].string, 1, outfifo[i].len, fpOut);
    }
    printf("\nWrite to file completed\n");
    // free memory
    free(infifo);
    free(outfifo);
}

void callKernel(FIFO *infifo, FIFO *outfifo, int no_of_blocks, char* cl_filename, char* cl_kernelname)
{
    FILE *fp;
    char *source_str;
    size_t source_size;
    printf("Filename %s\nKernelname %s\n",cl_filename,cl_kernelname);

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

    if(err != CL_SUCCESS){
        perror("Couldn't get platforms\n");
        return;
    }

    /* Platform Info Start */  
    cl_platform_info param_name[5] = {CL_PLATFORM_PROFILE,
                                      CL_PLATFORM_VERSION,
                                      CL_PLATFORM_NAME,
                                      CL_PLATFORM_VENDOR, 
                                      CL_PLATFORM_EXTENSIONS};
    size_t param_value_size;

    printf("\nPlatform ID: %d\n", 1);
    for(int j = 0; j < 5; ++j) {
        // Getting size of param.
        err = clGetPlatformInfo(cpPlatform, param_name[j], 0, NULL, &param_value_size );
        char *param_value = (char*) malloc(sizeof(char) * param_value_size);
        // Getting param info.
        err = clGetPlatformInfo(cpPlatform, param_name[j], param_value_size, param_value,NULL );
        printf("%s\n", param_value);
        free(param_value);
    }
    printf("\n\n");

    err = clGetDeviceIDs(cpPlatform, CL_DEVICE_TYPE_GPU, 1, &dev_id, NULL);

    if(err != CL_SUCCESS) {
        perror("Not able to get device ID\n");
        printf("Error Code: %d\n", err);
        return;
    }

    context = clCreateContext(0, 1, &dev_id, NULL, NULL, &err);

    if(err != CL_SUCCESS) {
        perror("Problem creating context\n");
        printf("Error Code: %d\n", err);
        return;
    }

    queue = clCreateCommandQueueWithProperties(context, dev_id, NULL, &err);
    if(err != CL_SUCCESS) {
        perror("Problem creating command queue\n");
        printf("Error Code: %d\n", err);
        return;
    }

    program = clCreateProgramWithSource(context, 1, (const char**)&source_str, (const size_t*)&source_size, &err);
    if(err != CL_SUCCESS) {
        perror("Problem creating program\n");
        printf("Error Code: %d\n", err);
        return;
    }

    clBuildProgram(program, 0, NULL, NULL, NULL, NULL);
    if(err != CL_SUCCESS) {
        perror("Problem building program executable.\n");
        printf("Error Code: %d\n", err);
        if(err == CL_BUILD_PROGRAM_FAILURE) {
            size_t log_siz;
            clGetProgramBuildInfo(program, dev_id, CL_PROGRAM_BUILD_LOG, 0, NULL, &log_siz );
            char *log = (char *) malloc(log_siz);
            clGetProgramBuildInfo(program, dev_id, CL_PROGRAM_BUILD_LOG, log_siz, log, NULL);
            printf("%s\n", log);
        }
        return;
    }

    size_t len = 0;
    cl_int ret = CL_SUCCESS;
    ret = clGetProgramBuildInfo(program, dev_id, CL_PROGRAM_BUILD_LOG, 0, NULL, &len);
    char *buffer = calloc(len, sizeof(char));
    ret = clGetProgramBuildInfo(program, dev_id, CL_PROGRAM_BUILD_LOG, len, buffer, NULL);
    printf("Build info\n%s\n", buffer);

    kernel = clCreateKernel(program, "EncodeLZSS", &err);
    if(err != CL_SUCCESS) {
        perror("Problem creating kernel.\n");
        printf("Error Code: %d\n", err);
        return;
    }

    d_inf = clCreateBuffer(context, CL_MEM_READ_ONLY, bytes, NULL, &err);
    if(err != CL_SUCCESS) {
        perror("Problem creating buffer d_inf.\n");
        printf("Error Code: %d\n", err);
        return;
    }

    d_outf = clCreateBuffer(context, CL_MEM_READ_ONLY, bytes, NULL, &err);
    if(err != CL_SUCCESS) {
        perror("Problem creating buffer d_b.\n");
        printf("Error Code: %d\n", err);
        return;
    }

    err = clEnqueueWriteBuffer(queue, d_inf, CL_TRUE, 0, bytes, infifo, 0, NULL, NULL);
    //err |= clEnqueueWriteBuffer(queue, d_outf, CL_TRUE, 0, bytes, outfifo, 0, NULL, NULL);
    if(err != CL_SUCCESS) {
        perror("Problem enqueing writes.\n");
        return;
    }

    err = clSetKernelArg(kernel, 0, sizeof(cl_mem), &d_inf);
    err |= clSetKernelArg(kernel, 1, sizeof(cl_mem), &d_outf);
    err |= clSetKernelArg(kernel, 2, sizeof(unsigned int), &no_of_blocks);
    err |= clSetKernelArg(kernel, 3, sizeof(unsigned int), &window_size);
    err |= clSetKernelArg(kernel, 4, sizeof(unsigned char)*window_size, NULL);
    err |= clSetKernelArg(kernel, 5, sizeof(unsigned char)*MAX_CODED, NULL);
    if(err != CL_SUCCESS) {
        perror("Problem setting arguments.\n");
        printf("Error Code: %d\n", err);
        return;
    }

    err = clEnqueueNDRangeKernel(queue, kernel, 1, NULL, &globalSize, &localSize, 0, NULL, NULL);
    if(err != CL_SUCCESS) {
        perror("Problem enqueing kernel.\n");
        printf("Error Code: %d\n", err);
        return;
    }

    err = clFinish(queue);
    if(err != CL_SUCCESS) {
        perror("Problem with CL Finish.\n");
        printf("Error Code: %d\n", err);
        return;
    }

    clEnqueueReadBuffer(queue, d_outf, CL_TRUE, 0, bytes, outfifo, 0, NULL, NULL);
    if(err != CL_SUCCESS) {
        perror("Problem reading from buffer.\n");
        printf("Error Code: %d\n", err);
        return;
    }

    clReleaseMemObject(d_inf);
    clReleaseMemObject(d_outf);
    clReleaseProgram(program);
    clReleaseKernel(kernel);
    clReleaseCommandQueue(queue);
    clReleaseContext(context);
}
