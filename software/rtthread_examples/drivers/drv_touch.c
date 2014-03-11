#include <stdbool.h>

#include "board.h"
#include "drv_touch.h"

/*
TOUCH INT: P0_13
*/
#define IS_TOUCH_UP()     (LPC_GPIO0->PIN&(0x01<<13))

#include <rtthread.h>
#include <rtdevice.h>
#include <rtgui/rtgui.h>
#include <rtgui/event.h>
#include <rtgui/touch.h>

/*
7  6 - 4  3      2     1-0
s  A2-A0 MODE SER/DFR PD1-PD0
*/
/* bit[1:0] power-down */
#define POWER_MODE0     (0) /* Power-Down Between Conversions. When */
/* each conversion is finished, the converter */
/* enters a low-power mode. At the start of the */
/* next conversion, the device instantly powers up */
/* to full power. There is no need for additional */
/* delays to ensure full operation, and the very first */
/* conversion is valid. The Y? switch is on when in */
/* power-down.*/
#define POWER_MODE1     (1) /* Reference is off and ADC is on. */
#define POWER_MODE2     (2) /* Reference is on and ADC is off. */
#define POWER_MODE3     (3) /* Device is always powered. Reference is on and */
/* ADC is on. */
/* bit[2] SER/DFR */
#define DIFFERENTIAL    (0<<2)
#define SINGLE_ENDED    (1<<2)
/* bit[3] mode */
#define MODE_12BIT      (0<<3)
#define MODE_8BIT       (1<<3)
/* bit[6:4] differential mode */
#define MEASURE_X       (((1<<2) | (0<<1) | (1<<0))<<4)
#define MEASURE_Y       (((0<<2) | (0<<1) | (1<<0))<<4)
#define MEASURE_Z1      (((0<<2) | (1<<1) | (1<<0))<<4)
#define MEASURE_Z2      (((1<<2) | (0<<1) | (0<<0))<<4)
/* bit[7] start */
#define START           (1<<7)

/* X Y change. */
#define TOUCH_MSR_X     (START | MEASURE_X | MODE_12BIT | DIFFERENTIAL | POWER_MODE0)
#define TOUCH_MSR_Y     (START | MEASURE_Y | MODE_12BIT | DIFFERENTIAL | POWER_MODE0)

#define MIN_X_DEFAULT   0x7bd
#define MAX_X_DEFAULT   0x20
#define MIN_Y_DEFAULT   0x53
#define MAX_Y_DEFAULT   0x79b
#define SAMP_CNT 8                              //the adc array size
#define SAMP_CNT_DIV2 4                         //the middle of the adc array
#define SH   20                                 // Valve value
struct rtgui_touch_device
{
    struct rt_device parent;

    rt_uint16_t x, y;

    struct rt_spi_device *spi_device;
    struct rt_event event;
};
static struct rtgui_touch_device* touch = RT_NULL;

rt_inline void touch_int_enable(rt_bool_t);

#define X_WIDTH 480
#define Y_WIDTH 272

static void rtgui_touch_calculate(void)
{
    rt_uint16_t adc_x, adc_y;
    if (touch != RT_NULL)
    {
        /* read touch */
        {
            uint8_t i, j, k, min;
            uint16_t temp;
            rt_uint16_t tmpxy[2][SAMP_CNT];
            uint8_t send_buffer[1];
            uint8_t recv_buffer[2];

            for (i = 0; i < SAMP_CNT; i++)
            {
                send_buffer[0] = TOUCH_MSR_X;
                rt_spi_send_then_recv(touch->spi_device,
                                      send_buffer,
                                      1,
                                      recv_buffer,
                                      2);
                tmpxy[0][i]  = (((recv_buffer[0] & 0x7F) << 8) | recv_buffer[1]);
                tmpxy[0][i] >>= 3;
                send_buffer[0] = TOUCH_MSR_Y;
                rt_spi_send_then_recv(touch->spi_device,
                                      send_buffer,
                                      1,
                                      recv_buffer,
                                      2);

                tmpxy[1][i]  = (((recv_buffer[0] & 0x7F) << 8) | recv_buffer[1]);
                tmpxy[1][i] >>= 3;
            }
            send_buffer[0] = 1 << 7;
            rt_spi_send(touch->spi_device, send_buffer, 1);

            /* calculate average */
            {
                rt_uint32_t total_x = 0;
                rt_uint32_t total_y = 0;

                for (k = 0; k < 2; k++)
                {
                    // sorting the ADC value
                    for (i = 0; i < SAMP_CNT - 1; i++)
                    {
                        min = i;
                        for (j = i + 1; j < SAMP_CNT; j++)
                        {
                            if (tmpxy[k][min] > tmpxy[k][j])
                                min = j;
                        }
                        temp = tmpxy[k][i];
                        tmpxy[k][i] = tmpxy[k][min];
                        tmpxy[k][min] = temp;
                    }
                    //check value for Valve value
                    if ((tmpxy[k][SAMP_CNT_DIV2 + 1] - tmpxy[k][SAMP_CNT_DIV2 - 2]) > SH)
                    {
                        return;
                    }
                }
                total_x = tmpxy[0][SAMP_CNT_DIV2 - 2] + tmpxy[0][SAMP_CNT_DIV2 - 1] + tmpxy[0][SAMP_CNT_DIV2] + tmpxy[0][SAMP_CNT_DIV2 + 1];
                total_y = tmpxy[1][SAMP_CNT_DIV2 - 2] + tmpxy[1][SAMP_CNT_DIV2 - 1] + tmpxy[1][SAMP_CNT_DIV2] + tmpxy[1][SAMP_CNT_DIV2 + 1];

				/* calculate to get average value */
                adc_x = (total_x >> 2);
                adc_y = (total_y >> 2);

                // rt_kprintf("touch->x:%d touch->y:%d\r\n", adc_x, adc_y);
            } /* calculate average */
        } /* read touch */

		/* update x,y */
        touch->x = adc_x;
        touch->y = adc_y;
    }
}

rt_inline void touch_int_enable(rt_bool_t en)
{
    if (RT_TRUE == en)
    {
        /* enable P0.13 failling edge interrupt */
        LPC_GPIOINT->IO0IntEnF |= (0x01 << 13);
    }
    else
    {
        /* disable P0.13 failling edge interrupt */
        LPC_GPIOINT->IO0IntEnF &= ~(0x01 << 13);
    }
}

static void touch_int_config(void)
{
    /* P0.13 touch INT */
    {
        LPC_IOCON->P0_13 &= ~0x07;
        LPC_GPIO0->DIR &= ~(0x01 << 13);
    }
    /* Configure  EXTI  */
    LPC_GPIOINT->IO0IntEnF |= (0x01 << 13);
    //touch_int_enable(RT_TRUE);
    NVIC_SetPriority(GPIO_IRQn, ((0x01 << 3) | 0x01));
    NVIC_EnableIRQ(GPIO_IRQn);
}

/* RT-Thread Device Interface */
static rt_err_t touch_device_init(rt_device_t dev)
{
    uint8_t send;
    struct rtgui_touch_device *touch_device = (struct rtgui_touch_device *)dev;

    touch_int_config();

    send = START | DIFFERENTIAL | POWER_MODE0;
    rt_spi_send(touch_device->spi_device, &send, 1); /* enable touch interrupt */

    return RT_EOK;
}

static void touch_thread_entry(void *parameter)
{
    rt_bool_t touch_down = RT_FALSE;
    rt_uint32_t event_value;

    while (1)
    {
        if (rt_event_recv(&touch->event,
                          1,
                          RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                          RT_WAITING_FOREVER,
                          &event_value)
                == RT_EOK)
        {
            while (1)
            {
                if (IS_TOUCH_UP())
                {
                    if (touch_down != RT_TRUE)
                    {
                        touch_int_enable(RT_TRUE);
                        break;
                    }

					rtgui_touch_post(RTGUI_TOUCH_UP, touch->x, touch->y);

                    /* clean */
                    touch_down = RT_FALSE;
                    touch_int_enable(RT_TRUE);
                    break;
                } /* touch up */
                else /* touch down or move */
                {
					int type = RTGUI_TOUCH_DOWN;
                    if (touch_down == RT_FALSE)
                    {
                        rt_thread_delay(RT_TICK_PER_SECOND / 10);
                    }
                    else
                    {
                        rt_thread_delay(RT_TICK_PER_SECOND / 20);
						/* touch motion event */
						type = RTGUI_TOUCH_MOTION;
                    }

					/* check it again */
                    if (IS_TOUCH_UP()) continue;

                    /* calculation */
                    rtgui_touch_calculate();
					rtgui_touch_post(type, touch->x, touch->y);

                    touch_down = RT_TRUE;
                } /* touch down or move */
            } /* read touch */
        } /* event recv */
    } /* thread while(1) */
}

void GPIO_IRQHandler(void)
{
    if ((LPC_GPIOINT->IO0IntStatF >> 13) & 0x01)
    {
        /* disable interrupt */
        touch_int_enable(RT_FALSE);
        rt_event_send(&touch->event, 1);
        LPC_GPIOINT->IO0IntClr |= (1 << 13);
    }
}

rt_err_t rtgui_touch_hw_init(const char *spi_device_name)
{
    struct rt_spi_device *spi_device;
    struct rt_thread *touch_thread;
    spi_device = (struct rt_spi_device *)rt_device_find(spi_device_name);

    if (spi_device == RT_NULL)
    {
        rt_kprintf("spi device %s not found!\r\n", spi_device_name);
        return -RT_ENOSYS;
    }

    /* config spi */
    {
        struct rt_spi_configuration cfg;
        cfg.data_width = 8;
        cfg.mode = RT_SPI_MODE_0 | RT_SPI_MSB; /* SPI Compatible Modes 0 */
        cfg.max_hz = 125 * 1000; /* 125K */
        rt_spi_configure(spi_device, &cfg);
    }

    touch = (struct rtgui_touch_device *)rt_malloc(sizeof(struct rtgui_touch_device));
    if (touch == RT_NULL) return RT_ENOMEM; /* no memory yet */
    /* clear device structure */
    rt_memset(&(touch->parent), 0, sizeof(struct rt_device));

    rt_event_init(&touch->event, "touch", RT_IPC_FLAG_FIFO);

    touch->spi_device = spi_device;

    /* init device structure */
    touch->parent.type = RT_Device_Class_Miscellaneous;
    touch->parent.init = touch_device_init;
    touch->parent.control = RT_NULL;
    touch->parent.user_data = RT_NULL;

    touch_device_init(&(touch->parent));
    /* register touch device to RT-Thread */
    rt_device_register(&(touch->parent), "touch", RT_DEVICE_FLAG_RDWR);

    touch_thread = rt_thread_create("touch",
                                    touch_thread_entry, RT_NULL,
                                    1024, RTGUI_SVR_THREAD_PRIORITY - 1, 1);
    if (touch_thread != RT_NULL) rt_thread_startup(touch_thread);

    return RT_EOK;
}
