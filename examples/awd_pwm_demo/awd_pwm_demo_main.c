/****************************************************************************
 * apps/examples/awd_pwm_demo/awd_pwm_demo_main.c
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>

#include <nuttx/analog/adc.h>   /* ANIOC_TRIGGER, struct adc_msg_s */
#include <nuttx/analog/ioctl.h> /* ANIOC_TRIGGER, struct adc_msg_s */
#include <sys/types.h>
#include <sys/ioctl.h>

/* Board glue lives in the board; prototypes declared here to avoid including
 * a board header from apps/ (Nucleo-H563ZI only exports board.h).
 */
extern int board_awd_pwm_latch_init(int tim, uint8_t ch, uint8_t awd,
                                    uint8_t etf, bool etp,
                                    uint32_t arr, uint32_t ccr);
extern int board_awd_pwm_latch_arm(void);
extern int board_awd_pwm_latch_rearm(void);
extern int board_awd_pwm_latch_disarm(void);
extern bool board_awd_pwm_latch_tripped(void);

#ifndef CONFIG_EXAMPLES_AWD_PWM_DEMO_TIM
#define CONFIG_EXAMPLES_AWD_PWM_DEMO_TIM 1 /* 1 or 8 */
#endif
#ifndef CONFIG_EXAMPLES_AWD_PWM_DEMO_CH
#define CONFIG_EXAMPLES_AWD_PWM_DEMO_CH 1 /* 1..4 */
#endif
#ifndef CONFIG_EXAMPLES_AWD_PWM_DEMO_AWD
#define CONFIG_EXAMPLES_AWD_PWM_DEMO_AWD 1 /* 1..3 */
#endif
#ifndef CONFIG_EXAMPLES_AWD_PWM_DEMO_ETF
#define CONFIG_EXAMPLES_AWD_PWM_DEMO_ETF 0 /* 0..15 */
#endif
#ifndef CONFIG_EXAMPLES_AWD_PWM_DEMO_ETP
#define CONFIG_EXAMPLES_AWD_PWM_DEMO_ETP 0 /* 0/1 */
#endif
#ifndef CONFIG_EXAMPLES_AWD_PWM_DEMO_ARR
#define CONFIG_EXAMPLES_AWD_PWM_DEMO_ARR 0xFFFF
#endif
#ifndef CONFIG_EXAMPLES_AWD_PWM_DEMO_CCR
#define CONFIG_EXAMPLES_AWD_PWM_DEMO_CCR 1 /* >0 for PWM2 */
#endif
#ifndef CONFIG_EXAMPLES_AWD_PWM_DEMO_ADCDEVPATH
#define CONFIG_EXAMPLES_AWD_PWM_DEMO_ADCDEVPATH "/dev/adc0"
#endif

/* Small on-stack drain chunk to keep DMA happy */
#ifndef CONFIG_EXAMPLES_AWD_PWM_DEMO_DRAIN_CHUNK
#define CONFIG_EXAMPLES_AWD_PWM_DEMO_DRAIN_CHUNK 64 /* struct adc_msg_s entries */
#endif

/* Drain any ready ADC samples without blocking */
static inline void drain_adc_nonblocking(int adcfd)
{
  if (adcfd < 0)
    return;

  struct pollfd p = {.fd = adcfd, .events = POLLIN, .revents = 0};
  int pr = poll(&p, 1, 0); /* 0ms: just a quick check */
  if (pr > 0 && (p.revents & POLLIN))
  {
    struct adc_msg_s buf[CONFIG_EXAMPLES_AWD_PWM_DEMO_DRAIN_CHUNK];
    /* Read and discard whatever is available (bounded by our tiny buffer) */
    (void)read(adcfd, buf, sizeof(buf));
  }
}

/* Wait for ENTER while draining ADC so DMA/IRQ doesn't storm */
static void wait_enter_while_draining(const char *prompt, int adcfd)
{
  printf("%s\n", prompt);

  struct pollfd pfds[2];
  pfds[0].fd = 0; /* stdin */
  pfds[0].events = POLLIN;
  pfds[1].fd = adcfd; /* adc */
  pfds[1].events = POLLIN;

  for (;;)
  {
    int pr = poll(pfds, 2, 50); /* 50 ms tick */
    if (pr > 0)
    {
      if (pfds[1].revents & POLLIN)
      {
        drain_adc_nonblocking(adcfd);
      }
      if (pfds[0].revents & POLLIN)
      {
        /* Eat the line and return */
        char throwaway[16];
        (void)read(0, throwaway, sizeof(throwaway));
        return;
      }
    }
    else
    {
      /* timeout: still drain periodically */
      drain_adc_nonblocking(adcfd);
    }
  }
}

/* Open ADC non-blocking and trigger conversions; keep fd open */
static int open_and_start_adc(const char *devpath)
{
  int fd = open(devpath, O_RDONLY | O_NONBLOCK);
  if (fd < 0)
  {
    printf("open(%s) failed: %d\n", devpath, errno);
    return -errno;
  }

  if (ioctl(fd, ANIOC_TRIGGER, 0) < 0)
  {
    int e = errno;
    printf("ANIOC_TRIGGER failed: %d\n", e);
    /* Keep fd open anyway in case ADC was already running from board init */
  }
  return fd;
}

int main(int argc, char *argv[])
{
  const int tim = CONFIG_EXAMPLES_AWD_PWM_DEMO_TIM;
  const uint8_t ch = CONFIG_EXAMPLES_AWD_PWM_DEMO_CH;
  const uint8_t awd = CONFIG_EXAMPLES_AWD_PWM_DEMO_AWD;
  const uint8_t etf = CONFIG_EXAMPLES_AWD_PWM_DEMO_ETF;
  const bool etp = CONFIG_EXAMPLES_AWD_PWM_DEMO_ETP != 0;
  const uint32_t arr = CONFIG_EXAMPLES_AWD_PWM_DEMO_ARR;
  const uint32_t ccr = CONFIG_EXAMPLES_AWD_PWM_DEMO_CCR;

  printf("awd_pwm_demo: TIM%d CH%u <= AWD%u (PWM2/CEN=0), ARR=0x%08lx CCR=%lu\n",
         tim, ch, awd, (unsigned long)arr, (unsigned long)ccr);

  /* Start ADC (DMA+continuous set up in board code). Keep fd open. */

  int adcfd = open_and_start_adc(CONFIG_EXAMPLES_AWD_PWM_DEMO_ADCDEVPATH);

  if (board_awd_pwm_latch_init(tim, ch, awd, etf, etp, arr, ccr) < 0)
  {
    printf("board_awd_pwm_latch_init failed\n");
    if (adcfd >= 0)
      close(adcfd);
    return EXIT_FAILURE;
  }

  wait_enter_while_draining("Press ENTER to ARM (drive HIGH). Trip AWD to latch LOW.", adcfd);

  if (board_awd_pwm_latch_arm() < 0)
  {
    printf("arm failed\n");
    if (adcfd >= 0)
      close(adcfd);
    return EXIT_FAILURE;
  }

  printf("Armed. Output should be HIGH now. Waiting for AWD...\n");

  /* Wait for AWD trip; keep draining ADC so DMA doesn't flood IRQs */
  while (!board_awd_pwm_latch_tripped())
  {
    /* Drain any pending samples and nap briefly */

    drain_adc_nonblocking(adcfd);
    struct pollfd p = {.fd = adcfd, .events = 0, .revents = 0};
    (void)poll(&p, 1, 25); /* ~40Hz loop, low CPU */
  }

  printf("\nAWD TRIPPED: output latched LOW (CNT reset).\n");

  for (;;)
  {
    wait_enter_while_draining("Press ENTER to RE-ARM (drive HIGH again). Ctrl-C to exit.", adcfd);
    if (board_awd_pwm_latch_rearm() < 0)
    {
      printf("re-arm failed\n");
    }
  }
}
