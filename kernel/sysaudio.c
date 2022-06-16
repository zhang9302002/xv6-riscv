#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "sound.h"
#include "spinlock.h"
#include "user/common.h"
#include "proc.h"

static struct soundNode audiobuf[3];
static struct coreBuf corebuf;
int datacount;
int bufcount;
int audiosize; // new data size
char buf[4096];
int isdecoding = 0;
int ismp3decoding = 0;
int ispaused = 0;
int putmask[9]={0x0, 0x1, 0x3, 0x7, 0xf, 0x1f, 0x3f, 0x7f, 0xff};
struct snd {
    struct spinlock lock;
    uint tag;
};
struct decode {
    struct spinlock lock;
    uint nread;
    uint nwrite;
};
struct snd sndlock;
struct decode decodelock, mp3lock;


int sys_setSampleRate(void)
{
    corebuf.buf_bit_idx=8;
    corebuf.totbit=0;
    corebuf.buf_byte_idx=0;
//    printf("sys_setaudiosmprate\n");
    int rate, i;
    //获取系统的第0个参数
    if (argint(0, &rate) < 0)
        return -1;

    datacount = 0;
    bufcount = 0;
    //将soundNode清空并置为已处理状态
    for (i = 0; i < 3; i++)
    {
        memset(&audiobuf[i], 0, sizeof(struct soundNode));
        audiobuf[i].flag = PROCESSED;
    }
    //audio.c设置采样率
    setSoundSampleRate(rate);
    return 0;
}

int
sys_wavdecode(void)
{
    printf("sys_wavdecode\n");
    //soundNode的数据大小
    int bufsize = DMA_BUF_NUM * DMA_BUF_SIZE;
    acquire(&decodelock.lock);
    while (isdecoding == 0) {
	    sleep(&decodelock.nread, &decodelock.lock);
    }
    release(&decodelock.lock);
    if (datacount == 0)
        memset(&audiobuf[bufcount], 0, sizeof(struct soundNode));
    //若soundNode的剩余大小大于数据大小，将数据写入soundNode中
//    printf("%d - %d > %d\n", bufsize, datacount, audiosize);
    if (bufsize - datacount > audiosize) {
        memmove(&audiobuf[bufcount].data[datacount], buf, audiosize);
        audiobuf[bufcount].flag = PCM_OUT | PROCESSED;
        datacount += audiosize;
    } else {
        int temp = bufsize - datacount,i;
        //soundNode存满后调用audioplay进行播放
        acquire(&sndlock.lock);
        while (ispaused == 1) {
            sleep(&sndlock.tag, &sndlock.lock);
        }
        release(&sndlock.lock);
        memmove(&audiobuf[bufcount].data[datacount], buf, temp);
        audiobuf[bufcount].flag = PCM_OUT;
        addSound(&audiobuf[bufcount]);
        int flag = 1;
        //寻找一个已经被处理的soundNode，将剩余数据戏写入
        while(flag == 1) {
            for (i = 0; i < 3; ++i) {
                if ((audiobuf[i].flag & PROCESSED) == PROCESSED) {
                    memset(&audiobuf[i], 0, sizeof(struct soundNode));
                    if (bufsize > audiosize - temp) {
                        memmove(&audiobuf[i].data[0], (buf + temp), (audiosize - temp));
                        audiobuf[i].flag = PCM_OUT | PROCESSED;
                        datacount = audiosize - temp;
                        bufcount = i;
                        flag = -1;
                        break;
                    } else {
                        memmove(&audiobuf[i].data[0], (buf + temp), bufsize);
                        temp = temp + bufsize;
                        audiobuf[i].flag = PCM_OUT;
                        addSound(&audiobuf[i]);
                    }
                }
            }
        }
    }
//    printf("decodee..");
    acquire(&decodelock.lock);
    isdecoding = 0;
    wakeup(&decodelock.nwrite);
    release(&decodelock.lock);
//    printf(" ..over\n");
    return 0;
}

int sys_endDecode(void)
{
    acquire(&mp3lock.lock);
    ismp3decoding = 0;
    wakeup(&mp3lock.nwrite);
    release(&mp3lock.lock);
    return 0;
}

int
sys_waitForDecode(void)
{
    acquire(&mp3lock.lock);
    while (ismp3decoding == 0)
    {
	sleep(&mp3lock.nread, &mp3lock.lock);
    }
    int ptr1, ptr2;
    argint(0, &ptr1);
    argint(1, &ptr2);
    memmove((char*)(uint64)ptr1, &corebuf.fr_ps, sizeof(struct frame_params));
    memmove((char*)(uint64)ptr2, &corebuf.III_side_info, sizeof(struct III_side_info_t));
    release(&mp3lock.lock);
    return 0;
}

int
sys_beginDecode(void)
{
    acquire(&mp3lock.lock);
    while (ismp3decoding) {
 	sleep(&mp3lock.nwrite, &mp3lock.lock);
    }
    int ptr1, ptr2;
    argint(0, &ptr1);
    argint(1, &ptr2);
    memmove(&corebuf.fr_ps, (char*)(uint64)ptr1, sizeof(struct frame_params));
    memmove(&corebuf.III_side_info, (char*)(uint64)ptr2, sizeof(struct III_side_info_t));

    ismp3decoding = 1;
    wakeup(&mp3lock.nread);
    release(&mp3lock.lock);
    acquire(&mp3lock.lock);
    while (ismp3decoding) {
        sleep(&mp3lock.nwrite, &mp3lock.lock);
    }
    while (TRUE)
	    printf("!!!\n");
    release(&mp3lock.lock);
    return 0;
}

int
sys_kwrite(void)
{
    uint64 buffer;
    //获取待播放的数据和数据大小
    acquire(&decodelock.lock);
    while (isdecoding) {
 	    sleep(&decodelock.nwrite, &decodelock.lock);
    }
    if (argint(1, &audiosize) < 0 || argaddr(0, &buffer) < 0)
        return -1;
    struct proc *pr = myproc();
    //将解码后的数据从用户空间拷贝进内核空间
    if(copyin(pr->pagetable, buf, buffer, audiosize) == -1)
        return -1;
    isdecoding = 1;
    wakeup(&decodelock.nread);
    release(&decodelock.lock);
    return 0;
}

int
sys_pause(void)
{
    sndlock.tag = 0;
    if (ispaused == 0)
	ispaused = 1;
    else {
	acquire(&sndlock.lock);
	ispaused = 0;
	wakeup(&sndlock.tag);
	release(&sndlock.lock);
    }
    return 0;
}

int sys_getCoreBuf(void)
{
	int type, para;
	argint(0, &type);
	argint(1, &para);
	unsigned long val=0;
	register int j = para;
	register int k, tmp;
	switch(type)
	{
	case 1:
		corebuf.buf[corebuf.offset % BUFSIZE] = para;
		corebuf.offset++;
		printf("haha %d\n", corebuf.offset);
		return 0;
	case 2:
		return corebuf.totbit;
	case 3:
	corebuf.totbit += para;
	while (j > 0) {
		if (!corebuf.buf_bit_idx) {
			corebuf.buf_bit_idx = 8;
			corebuf.buf_byte_idx++;
			if (corebuf.buf_byte_idx > corebuf.offset) {
				printf("%d %d hjw Buffer overflow !!\n", corebuf.buf_byte_idx, corebuf.offset);
				exit(0);
			}
		}
		k = MIN(j, corebuf.buf_bit_idx);
		tmp = buf[corebuf.buf_byte_idx%4096]&putmask[corebuf.buf_bit_idx];
		tmp = tmp >> (corebuf.buf_bit_idx-k);
		val |= tmp << (j-k);
		corebuf.buf_bit_idx -= k;
		j -= k;
	}
	return(val);
	case 4:
		corebuf.totbit -= para;
		corebuf.buf_bit_idx += para;
		while( corebuf.buf_bit_idx >= 8 ){
			corebuf.buf_bit_idx -= 8;
			corebuf.buf_byte_idx--;
		}
		return 0;
	case 5:
		corebuf.totbit -= para*8;
		corebuf.buf_byte_idx -= para;
		return 0;
	}
	return 0;
}
