/*
 * STM32F103 无线充电发射端单文件模板
 *
 * 硬件结构：
 *   STM32F103 的 TIM1 输出四路互补 PWM
 *   -> 两片半桥驱动芯片
 *   -> MOSFET 全桥逆变
 *   -> LCC 谐振补偿网络
 *   -> 发射线圈
 *
 * 使用提醒：
 *   1. 这是基于 HAL 库的 main.c 模板，需要配合 STM32F1 HAL 工程使用。
 *   2. 第一次上电不要直接接高母线电压，先用限流电源和低电压，例如 12V。
 *   3. 软件保护只能兜底，真正的过流保护最好用硬件比较器直接关断驱动或接 TIM1_BKIN。
 */

#include "stm32f1xx_hal.h"  /* HAL 库总头文件，必须在 HAL 工程中使用 */

#ifndef __HAL_TIM_MOE_ENABLE
#define __HAL_TIM_MOE_ENABLE(__HANDLE__)  ((__HANDLE__)->Instance->BDTR |= TIM_BDTR_MOE)   /* 兼容旧 HAL：打开高级定时器主输出 */
#endif

#ifndef __HAL_TIM_MOE_DISABLE
#define __HAL_TIM_MOE_DISABLE(__HANDLE__) ((__HANDLE__)->Instance->BDTR &= ~TIM_BDTR_MOE)  /* 兼容旧 HAL：关闭高级定时器主输出 */
#endif

/* =========================
 * 1) 赛场快速修改参数区
 * =========================
 * 到赛场后优先改这里。下面的宏把未知硬件参数集中到文件开头。
 */

/* -------- 功率级开关频率 --------
 * 全桥输出 50% 占空比方波。
 * LCC 发射端通常通过“调频”改变输出功率。
 */
#define WPT_FREQ_START_HZ          180000UL  /* 启动频率，未知谐振点时建议先离谐振点远一点 */
#define WPT_FREQ_MIN_HZ             80000UL  /* 允许的最低开关频率 */
#define WPT_FREQ_MAX_HZ            250000UL  /* 允许的最高开关频率 */
#define WPT_FREQ_STEP_HZ              500UL  /* 闭环控制时每次调整的频率步长 */
#define WPT_SWEEP_STEP_HZ            1000UL  /* 扫频寻找负载时每次调整的频率步长 */

/* 如果不知道谐振点在哪一侧，先低压扫频观察。
 * 多数系统靠近谐振点电流会变大，但“升频变大”还是“降频变大”取决于 LCC 参数。
 * 这里先假设降低频率会增大功率。
 */
#define WPT_POWER_UP_BY_LOWER_FREQ       1   /* 1 表示降频增功率，0 表示升频增功率 */

/* ========================= 引脚配置宏：先改这里 =========================
 * 下面这些宏只是“引脚编号定义”。
 * 真正把引脚配置成复用输出、模拟输入、普通输出的代码在 MX_GPIO_Init()。
 */

/* -------- TIM1 PWM 输出引脚，默认使用 STM32F103 常见引脚 --------
 * TIM1_CH1  PA8   -> 半桥 A 高边输入
 * TIM1_CH1N PB13  -> 半桥 A 低边输入
 * TIM1_CH2  PA9   -> 半桥 B 高边输入
 * TIM1_CH2N PB14  -> 半桥 B 低边输入
 * TIM1_BKIN PB12  -> 硬件故障关断输入
 */
#define WPT_GPIO_TIM1_PORT_A       GPIOA        /* PA8、PA9 所在端口 */
#define WPT_GPIO_TIM1_PORT_B       GPIOB        /* PB12、PB13、PB14 所在端口 */
#define WPT_PIN_TIM1_CH1           GPIO_PIN_8   /* PA8：半桥 A 高边 */
#define WPT_PIN_TIM1_CH2           GPIO_PIN_9   /* PA9：半桥 B 高边 */
#define WPT_PIN_TIM1_CH1N          GPIO_PIN_13  /* PB13：半桥 A 低边 */
#define WPT_PIN_TIM1_CH2N          GPIO_PIN_14  /* PB14：半桥 B 低边 */
#define WPT_PIN_TIM1_BKIN          GPIO_PIN_12  /* PB12：TIM1 硬件关断输入 */

/* -------- TIM1_BKIN 硬件关断 --------
 * 如果 PB12 没有接比较器或故障电路，保持为 0。
 * 如果 PB12 悬空而这里设成 1，PWM 可能会一直出不来。
 */
#define WPT_USE_TIM1_BKIN          0   /* 0 表示暂不启用 BKIN，1 表示启用 BKIN */
#define WPT_TIM1_BKIN_ACTIVE_HIGH  1   /* 1 表示高电平触发故障，0 表示低电平触发故障 */

/* -------- 半桥驱动使能引脚 --------
 * 如果驱动芯片没有 EN、SD、INH 之类的使能脚，把 WPT_USE_DRIVER_EN 改成 0。
 */
#define WPT_USE_DRIVER_EN          1           /* 1 表示使用驱动使能脚 */
#define WPT_DRIVER_EN_PORT         GPIOB       /* 驱动使能脚所在端口 */
#define WPT_DRIVER_EN_PIN          GPIO_PIN_0  /* PB0：驱动使能脚 */
#define WPT_DRIVER_EN_ACTIVE_HIGH  1           /* 1 表示高电平使能驱动 */

/* 多数半桥驱动芯片的 HIN、LIN 输入都是高有效。
 * 如果你的驱动板前面加了反相器，或者输入低有效，就改这里。
 */
#define WPT_HIGH_SIDE_ACTIVE_HIGH  1  /* 高边输入是否高有效 */
#define WPT_LOW_SIDE_ACTIVE_HIGH   1  /* 低边输入是否高有效 */

/* -------- 死区时间 --------
 * 死区太小：上下管可能直通。
 * 死区太大：损耗增加，波形变差。
 * 初次调试常用 300ns 到 800ns。
 */
#define WPT_DEADTIME_NS            500UL  /* TIM1 自动插入的互补 PWM 死区时间 */

/* -------- ADC 输入引脚和通道 --------
 * 默认分配：
 *   PA0 ADC1_IN0：母线电压
 *   PA1 ADC1_IN1：母线电流
 *   PA2 ADC1_IN2：线圈或谐振电流
 *   PA3 ADC1_IN3：谐振电容或线圈端电压
 *   PA4 ADC1_IN4：温度传感器或 NTC 分压
 */
#define WPT_ADC_BUS_V_CH           ADC_CHANNEL_0  /* 母线电压 ADC 通道 */
#define WPT_ADC_BUS_I_CH           ADC_CHANNEL_1  /* 母线电流 ADC 通道 */
#define WPT_ADC_COIL_I_CH          ADC_CHANNEL_2  /* 线圈电流 ADC 通道 */
#define WPT_ADC_RES_V_CH           ADC_CHANNEL_3  /* 谐振电压 ADC 通道 */
#define WPT_ADC_TEMP_CH            ADC_CHANNEL_4  /* 温度 ADC 通道 */

/* -------- ADC 换算常数 -------- */
#define WPT_ADC_VREF_MV            3300UL  /* ADC 参考电压，单位 mV，常见为 3.3V */
#define WPT_ADC_MAX_COUNT          4095UL  /* 12 位 ADC 最大计数值 */

/* -------- 电压采样分压电阻 --------
 * 实际电压 = ADC 引脚电压 * (上电阻 + 下电阻) / 下电阻。
 * 例子：100k + 4.7k 分压，48V 输入时 ADC 约为 2.15V。
 */
#define WPT_BUS_V_R_HIGH_OHM       100000UL  /* 母线电压分压上电阻 */
#define WPT_BUS_V_R_LOW_OHM          4700UL  /* 母线电压分压下电阻 */
#define WPT_RES_V_R_HIGH_OHM       100000UL  /* 谐振电压分压上电阻 */
#define WPT_RES_V_R_LOW_OHM          4700UL  /* 谐振电压分压下电阻 */

/* -------- 电流采样换算 --------
 * 实际电流 mA = 距离零点的 ADC 电压 mV * 每 mV 对应的 mA。
 * 如果是采样电阻加运放，零点通常是 0mV。
 * 如果是 ACS712 这类霍尔传感器，零点通常约为 1650mV。
 */
#define WPT_BUS_I_MA_PER_MV        10UL  /* 母线电流传感器比例 */
#define WPT_COIL_I_MA_PER_MV       10UL  /* 线圈电流传感器比例 */
#define WPT_BUS_I_ZERO_MV          0UL   /* 母线电流 0A 对应 ADC 电压 */
#define WPT_COIL_I_ZERO_MV         0UL   /* 线圈电流 0A 对应 ADC 电压 */

/* -------- 保护阈值 --------
 * 第一次上电必须保守设置。
 * 如果采样比例还没标定，可以先用下面的 raw 原始值限制保护。
 */
#define WPT_BUS_OVERVOLT_MV        60000UL  /* 母线过压阈值 */
#define WPT_RES_OVERVOLT_MV        90000UL  /* 谐振端过压阈值 */
#define WPT_BUS_OVERCURRENT_MA      5000UL  /* 母线过流阈值 */
#define WPT_COIL_OVERCURRENT_MA     8000UL  /* 线圈过流阈值 */

/* raw 阈值是 ADC 原始计数保护。
 * 4095 表示基本不限制。
 * 调试早期可以把电流 raw 阈值调低，先保证不会炸管。
 */
#define WPT_BUS_V_RAW_MAX          4095UL  /* 母线电压 ADC 原始值上限 */
#define WPT_BUS_I_RAW_MAX          4095UL  /* 母线电流 ADC 原始值上限 */
#define WPT_COIL_I_RAW_MAX         4095UL  /* 线圈电流 ADC 原始值上限 */
#define WPT_RES_V_RAW_MAX          4095UL  /* 谐振电压 ADC 原始值上限 */
#define WPT_TEMP_RAW_MAX           4095UL  /* 温度 ADC 原始值上限，没接温度时保持 4095 */

/* -------- 控制目标 --------
 * 没有接收端通信时，先用线圈电流或母线电流做粗略闭环。
 */
#define WPT_TARGET_COIL_CURRENT_MA 3000UL  /* 目标线圈电流 */
#define WPT_TARGET_HYST_MA          200UL  /* 控制滞环，避免频率来回抖动 */

/* -------- 时间参数 -------- */
#define WPT_CONTROL_PERIOD_MS      5UL   /* 正常运行控制周期 */
#define WPT_SOFTSTART_PERIOD_MS    20UL  /* 软启动或扫频的步进周期 */
#define WPT_FAULT_RESTART_MS       0UL   /* 0 表示故障锁死；非 0 表示延时自动重启 */

/* -------- 启动策略 --------
 * 0：直接从 WPT_FREQ_START_HZ 启动，然后进入闭环。
 * 1：从最高频向最低频扫频，通过线圈电流判断是否有负载。
 */
#define WPT_USE_STARTUP_SWEEP      1      /* 1 表示启用启动扫频 */
#define WPT_SWEEP_CURRENT_MA       500UL  /* 扫频时判断接收端存在的线圈电流阈值 */

/* =========================
 * 2) 全局变量和 HAL 句柄
 * =========================
 */

TIM_HandleTypeDef htim1;  /* TIM1 句柄，用于四路互补 PWM */
ADC_HandleTypeDef hadc1;  /* ADC1 句柄，用于读取电压、电流、温度 */

typedef enum
{
    WPT_STATE_IDLE = 0,     /* 空闲状态：关闭逆变 */
    WPT_STATE_SWEEP,        /* 扫频状态：寻找接收端或负载 */
    WPT_STATE_SOFTSTART,    /* 软启动状态：安全启动 PWM */
    WPT_STATE_RUN,          /* 运行状态：闭环调频 */
    WPT_STATE_FAULT         /* 故障状态：关闭逆变并等待处理 */
} WptState;

typedef struct
{
    uint16_t bus_v_raw;   /* 母线电压 ADC 原始值 */
    uint16_t bus_i_raw;   /* 母线电流 ADC 原始值 */
    uint16_t coil_i_raw;  /* 线圈电流 ADC 原始值 */
    uint16_t res_v_raw;   /* 谐振电压 ADC 原始值 */
    uint16_t temp_raw;    /* 温度 ADC 原始值 */

    uint32_t bus_v_mv;    /* 换算后的母线电压，单位 mV */
    uint32_t bus_i_ma;    /* 换算后的母线电流，单位 mA */
    uint32_t coil_i_ma;   /* 换算后的线圈电流，单位 mA */
    uint32_t res_v_mv;    /* 换算后的谐振电压，单位 mV */
} WptAdc;

static volatile WptState g_state = WPT_STATE_IDLE;  /* 当前状态机状态 */
static volatile uint8_t g_break_fault = 0;          /* BKIN 硬件故障标志 */
static volatile uint8_t g_pwm_running = 0;          /* PWM 是否已经启动 */
static uint32_t g_freq_hz = WPT_FREQ_START_HZ;      /* 当前输出频率 */
static uint32_t g_last_control_ms = 0;              /* 上一次闭环控制时间 */
static uint32_t g_last_softstart_ms = 0;            /* 上一次软启动或扫频时间 */
static uint32_t g_fault_ms = 0;                     /* 进入故障状态的时间 */
static WptAdc g_adc;                                /* 全局 ADC 采样结果 */

/* =========================
 * 3) 函数声明
 * =========================
 */

void SystemClock_Config(void);                                      /* 系统时钟配置 */
static void MX_GPIO_Init(void);                                     /* GPIO 引脚配置 */
static void MX_ADC1_Init(void);                                     /* ADC1 初始化 */
static void MX_TIM1_Init(void);                                     /* TIM1 PWM 初始化 */

static uint8_t TIM1_DeadTimeCodeFromNs(uint32_t deadtime_ns);       /* 把 ns 死区换成 TIM1 编码 */
static uint32_t TIM1_GetClockHz(void);                              /* 获取 TIM1 实际时钟 */
static void DriverEnable(uint8_t enable);                           /* 控制驱动芯片使能脚 */
static void InverterStart(void);                                    /* 启动全桥逆变 */
static void InverterStop(void);                                     /* 停止全桥逆变 */
static void InverterSetFrequency(uint32_t freq_hz);                 /* 设置逆变频率 */
static uint16_t ADC_ReadChannel(uint32_t channel);                  /* 读取单个 ADC 通道 */
static uint32_t ADC_RawToMv(uint16_t raw);                          /* ADC 原始值转 mV */
static uint32_t ScaleDividerMv(uint32_t adc_mv, uint32_t r_high, uint32_t r_low); /* 分压还原 */
static void ReadAllAdc(WptAdc *adc);                                /* 读取所有采样量 */
static uint8_t CheckFault(const WptAdc *adc);                       /* 检查是否故障 */
static void ClearFaults(void);                                      /* 清除故障标志 */
static void ControlTask(void);                                      /* 周期控制任务 */
static void SweepTask(void);                                        /* 扫频任务 */
static void SoftStartTask(void);                                    /* 软启动任务 */
static void RunTask(void);                                          /* 正常运行控制任务 */
static void Error_Handler(void);                                    /* 错误处理 */

/* =========================
 * 4) 主函数
 * =========================
 */

int main(void)
{
    HAL_Init();             /* 初始化 HAL 库，配置 SysTick 等基础功能 */
    SystemClock_Config();   /* 配置系统时钟，默认使用外部 8MHz 晶振倍频到 72MHz */

    MX_GPIO_Init();         /* 配置所有 GPIO 引脚 */
    MX_ADC1_Init();         /* 初始化 ADC1 */
    MX_TIM1_Init();         /* 初始化 TIM1 互补 PWM */

    HAL_ADCEx_Calibration_Start(&hadc1);  /* F103 ADC 上电后建议校准，提高采样准确性 */

    DriverEnable(0);        /* 上电先关闭半桥驱动 */
    InverterStop();         /* 确保 PWM 和主输出都关闭 */
    ClearFaults();          /* 清除历史故障标志 */

#if WPT_USE_STARTUP_SWEEP
    g_state = WPT_STATE_SWEEP;       /* 启动后先进入扫频寻找负载 */
    g_freq_hz = WPT_FREQ_MAX_HZ;     /* 扫频从最高频开始，通常更安全 */
#else
    g_state = WPT_STATE_SOFTSTART;   /* 不扫频时进入软启动 */
    g_freq_hz = WPT_FREQ_START_HZ;   /* 使用配置的启动频率 */
#endif

    while (1)
    {
        ReadAllAdc(&g_adc);          /* 每轮循环先读取电压、电流、温度 */

        if (CheckFault(&g_adc))      /* 保护检查要高频执行，不能只放在慢速控制里 */
        {
            InverterStop();          /* 一旦故障，立刻关闭全桥输出 */
            g_fault_ms = HAL_GetTick(); /* 记录故障发生时间，给自动重启用 */
            g_state = WPT_STATE_FAULT;  /* 进入故障状态 */
        }

        switch (g_state)             /* 主状态机，根据状态执行不同任务 */
        {
        case WPT_STATE_IDLE:
            InverterStop();          /* 空闲状态保持输出关闭 */
            break;

        case WPT_STATE_SWEEP:
            SweepTask();             /* 扫频寻找接收端或负载 */
            break;

        case WPT_STATE_SOFTSTART:
            SoftStartTask();         /* 软启动 PWM */
            break;

        case WPT_STATE_RUN:
            ControlTask();           /* 正常运行时进行闭环调频 */
            break;

        case WPT_STATE_FAULT:
            InverterStop();          /* 故障状态持续保持输出关闭 */
#if WPT_FAULT_RESTART_MS > 0
            if ((HAL_GetTick() - g_fault_ms) > WPT_FAULT_RESTART_MS)  /* 到达自动重启延时 */
            {
                ClearFaults();       /* 清故障标志 */
                g_state = WPT_STATE_SWEEP;  /* 重新从扫频开始 */
                g_freq_hz = WPT_FREQ_MAX_HZ; /* 重启也从最高频开始 */
            }
#endif
            break;

        default:
            g_state = WPT_STATE_FAULT; /* 异常状态直接转故障，避免乱输出 */
            break;
        }
    }
}

/* =========================
 * 5) 全桥 PWM 控制
 * =========================
 */

static void InverterStart(void)
{
    if (g_pwm_running)       /* 如果 PWM 已经在运行，就不要重复启动 */
    {
        return;
    }

    DriverEnable(0);         /* 先关闭外部驱动，防止启动瞬间某一路先导通 */

    __HAL_TIM_ENABLE(&htim1); /* 启动 TIM1 计数器 */
    htim1.Instance->CCER |= TIM_CCER_CC1E | TIM_CCER_CC1NE |
                            TIM_CCER_CC2E | TIM_CCER_CC2NE; /* 一次性打开 CH1/CH1N/CH2/CH2N 四路输出 */
    __HAL_TIM_MOE_ENABLE(&htim1); /* 打开高级定时器主输出，否则互补 PWM 不会真正输出到引脚 */

    DriverEnable(1);         /* 四路 PWM 准备好后，再打开半桥驱动 */
    g_pwm_running = 1;       /* 记录 PWM 已运行 */
}

static void InverterStop(void)
{
    DriverEnable(0);         /* 停机先关外部驱动，速度比单纯停定时器更保险 */

    if (htim1.Instance == TIM1) /* 防止时钟初始化失败时误操作未初始化的句柄 */
    {
        __HAL_TIM_MOE_DISABLE(&htim1); /* 关闭 TIM1 主输出 */
        htim1.Instance->CCER &= ~(TIM_CCER_CC1E | TIM_CCER_CC1NE |
                                  TIM_CCER_CC2E | TIM_CCER_CC2NE); /* 关闭四路 PWM 通道输出 */
        __HAL_TIM_DISABLE(&htim1); /* 停止 TIM1 计数器 */
    }

    g_pwm_running = 0;       /* 记录 PWM 已停止 */
}

static void InverterSetFrequency(uint32_t freq_hz)
{
    uint32_t timer_clk;      /* TIM1 实际输入时钟 */
    uint32_t arr;            /* 自动重装载值，决定 PWM 周期 */

    if (freq_hz < WPT_FREQ_MIN_HZ)  /* 限制最低频率，防止跑出安全范围 */
    {
        freq_hz = WPT_FREQ_MIN_HZ;
    }
    if (freq_hz > WPT_FREQ_MAX_HZ)  /* 限制最高频率，防止跑出安全范围 */
    {
        freq_hz = WPT_FREQ_MAX_HZ;
    }

    timer_clk = TIM1_GetClockHz();          /* 获取 TIM1 时钟，通常为 72MHz */
    arr = (timer_clk / freq_hz) - 1UL;      /* ARR = 定时器时钟 / 目标频率 - 1 */
    if (arr < 10UL)                         /* ARR 太小会导致分辨率过低 */
    {
        arr = 10UL;
    }

    __HAL_TIM_SET_AUTORELOAD(&htim1, arr);              /* 设置 PWM 周期 */
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, arr / 2UL); /* CH1 保持 50% 占空比 */
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, arr / 2UL); /* CH2 保持 50% 占空比 */
    htim1.Instance->EGR = TIM_EGR_UG;                   /* 产生更新事件，让新 ARR/CCR 尽快生效 */

    g_freq_hz = freq_hz;                    /* 保存当前频率 */
}

/* =========================
 * 6) 状态机任务
 * =========================
 */

static void SweepTask(void)
{
    uint32_t now = HAL_GetTick(); /* 当前毫秒时间 */

    if ((now - g_last_softstart_ms) < WPT_SOFTSTART_PERIOD_MS) /* 扫频步进限速 */
    {
        return;
    }
    g_last_softstart_ms = now; /* 记录本次扫频时间 */

    InverterSetFrequency(g_freq_hz); /* 输出当前扫频频率 */
    InverterStart();                 /* 确保逆变器已经启动 */

    if (g_adc.coil_i_ma > WPT_SWEEP_CURRENT_MA) /* 线圈电流超过阈值，认为有接收端或负载 */
    {
        g_state = WPT_STATE_RUN;     /* 进入正常运行状态 */
        return;
    }

    if (g_freq_hz > (WPT_FREQ_MIN_HZ + WPT_SWEEP_STEP_HZ)) /* 还没扫到最低频 */
    {
        g_freq_hz -= WPT_SWEEP_STEP_HZ; /* 从高频向低频扫 */
    }
    else
    {
        g_freq_hz = WPT_FREQ_MAX_HZ; /* 没找到负载就回到最高频重新扫 */
    }
}

static void SoftStartTask(void)
{
    uint32_t now = HAL_GetTick(); /* 当前毫秒时间 */

    if ((now - g_last_softstart_ms) < WPT_SOFTSTART_PERIOD_MS) /* 软启动限速 */
    {
        return;
    }
    g_last_softstart_ms = now; /* 记录软启动时间 */

    InverterSetFrequency(g_freq_hz); /* 设置启动频率 */
    InverterStart();                 /* 打开全桥输出 */
    g_state = WPT_STATE_RUN;         /* 启动完成后进入运行状态 */
}

static void ControlTask(void)
{
    uint32_t now = HAL_GetTick(); /* 当前毫秒时间 */

    if ((now - g_last_control_ms) < WPT_CONTROL_PERIOD_MS) /* 控制周期限速 */
    {
        return;
    }
    g_last_control_ms = now; /* 记录本次控制时间 */

    RunTask();               /* 执行真正的闭环调频 */
}

static void RunTask(void)
{
    uint32_t target_low = WPT_TARGET_COIL_CURRENT_MA - WPT_TARGET_HYST_MA;  /* 电流目标下限 */
    uint32_t target_high = WPT_TARGET_COIL_CURRENT_MA + WPT_TARGET_HYST_MA; /* 电流目标上限 */

    if (g_adc.coil_i_ma < target_low) /* 线圈电流偏小，需要增大功率 */
    {
#if WPT_POWER_UP_BY_LOWER_FREQ
        if (g_freq_hz > (WPT_FREQ_MIN_HZ + WPT_FREQ_STEP_HZ)) /* 防止降到最低频以下 */
        {
            g_freq_hz -= WPT_FREQ_STEP_HZ; /* 假设降频会增大功率 */
        }
#else
        if (g_freq_hz < (WPT_FREQ_MAX_HZ - WPT_FREQ_STEP_HZ)) /* 防止升到最高频以上 */
        {
            g_freq_hz += WPT_FREQ_STEP_HZ; /* 假设升频会增大功率 */
        }
#endif
    }
    else if (g_adc.coil_i_ma > target_high) /* 线圈电流偏大，需要减小功率 */
    {
#if WPT_POWER_UP_BY_LOWER_FREQ
        if (g_freq_hz < (WPT_FREQ_MAX_HZ - WPT_FREQ_STEP_HZ)) /* 防止升到最高频以上 */
        {
            g_freq_hz += WPT_FREQ_STEP_HZ; /* 假设升频会减小功率 */
        }
#else
        if (g_freq_hz > (WPT_FREQ_MIN_HZ + WPT_FREQ_STEP_HZ)) /* 防止降到最低频以下 */
        {
            g_freq_hz -= WPT_FREQ_STEP_HZ; /* 假设降频会减小功率 */
        }
#endif
    }

    InverterSetFrequency(g_freq_hz); /* 把新的频率写入 TIM1 */
    InverterStart();                 /* 如果还没启动，确保启动；已启动则直接返回 */
}

/* =========================
 * 7) ADC 采样和保护判断
 * =========================
 */

static uint16_t ADC_ReadChannel(uint32_t channel)
{
    ADC_ChannelConfTypeDef sConfig; /* ADC 通道配置结构体 */
    uint16_t raw = 0;               /* ADC 原始采样值 */

    sConfig.Channel = channel;                    /* 选择要读取的 ADC 通道 */
    sConfig.Rank = ADC_REGULAR_RANK_1;            /* 单通道采样，固定为第 1 个转换 */
    sConfig.SamplingTime = ADC_SAMPLETIME_55CYCLES_5; /* 采样时间，源阻抗较大时不要太短 */

    if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) /* 配置当前 ADC 通道 */
    {
        return 0; /* 配置失败时返回 0，后续保护仍会继续运行 */
    }

    HAL_ADC_Start(&hadc1); /* 启动 ADC 转换 */
    if (HAL_ADC_PollForConversion(&hadc1, 2) == HAL_OK) /* 等待转换完成，超时 2ms */
    {
        raw = (uint16_t)HAL_ADC_GetValue(&hadc1); /* 读取 ADC 原始值 */
    }
    HAL_ADC_Stop(&hadc1); /* 停止 ADC，方便下次切换通道 */

    return raw; /* 返回本次采样结果 */
}

static uint32_t ADC_RawToMv(uint16_t raw)
{
    return ((uint32_t)raw * WPT_ADC_VREF_MV) / WPT_ADC_MAX_COUNT; /* ADC 原始值换算成引脚电压 mV */
}

static uint32_t ScaleDividerMv(uint32_t adc_mv, uint32_t r_high, uint32_t r_low)
{
    return (adc_mv * (r_high + r_low)) / r_low; /* 根据分压电阻还原真实电压 */
}

static void ReadAllAdc(WptAdc *adc)
{
    uint32_t bus_v_adc_mv;   /* 母线电压 ADC 引脚电压 */
    uint32_t bus_i_adc_mv;   /* 母线电流 ADC 引脚电压 */
    uint32_t coil_i_adc_mv;  /* 线圈电流 ADC 引脚电压 */
    uint32_t res_v_adc_mv;   /* 谐振电压 ADC 引脚电压 */
    uint32_t bus_i_diff_mv;  /* 母线电流采样距离零点的电压 */
    uint32_t coil_i_diff_mv; /* 线圈电流采样距离零点的电压 */

    adc->bus_v_raw = ADC_ReadChannel(WPT_ADC_BUS_V_CH);     /* 读取母线电压原始值 */
    adc->bus_i_raw = ADC_ReadChannel(WPT_ADC_BUS_I_CH);     /* 读取母线电流原始值 */
    adc->coil_i_raw = ADC_ReadChannel(WPT_ADC_COIL_I_CH);   /* 读取线圈电流原始值 */
    adc->res_v_raw = ADC_ReadChannel(WPT_ADC_RES_V_CH);     /* 读取谐振电压原始值 */
    adc->temp_raw = ADC_ReadChannel(WPT_ADC_TEMP_CH);       /* 读取温度原始值 */

    bus_v_adc_mv = ADC_RawToMv(adc->bus_v_raw);     /* 母线电压 ADC 原始值转 mV */
    bus_i_adc_mv = ADC_RawToMv(adc->bus_i_raw);     /* 母线电流 ADC 原始值转 mV */
    coil_i_adc_mv = ADC_RawToMv(adc->coil_i_raw);   /* 线圈电流 ADC 原始值转 mV */
    res_v_adc_mv = ADC_RawToMv(adc->res_v_raw);     /* 谐振电压 ADC 原始值转 mV */

    adc->bus_v_mv = ScaleDividerMv(bus_v_adc_mv, WPT_BUS_V_R_HIGH_OHM, WPT_BUS_V_R_LOW_OHM); /* 还原母线真实电压 */
    adc->res_v_mv = ScaleDividerMv(res_v_adc_mv, WPT_RES_V_R_HIGH_OHM, WPT_RES_V_R_LOW_OHM); /* 还原谐振端真实电压 */

    bus_i_diff_mv = (bus_i_adc_mv > WPT_BUS_I_ZERO_MV) /* 判断采样值在零点上方还是下方 */
                    ? (bus_i_adc_mv - WPT_BUS_I_ZERO_MV)
                    : (WPT_BUS_I_ZERO_MV - bus_i_adc_mv);
    coil_i_diff_mv = (coil_i_adc_mv > WPT_COIL_I_ZERO_MV) /* 取距离零点的绝对值，兼容双向电流传感器 */
                     ? (coil_i_adc_mv - WPT_COIL_I_ZERO_MV)
                     : (WPT_COIL_I_ZERO_MV - coil_i_adc_mv);

    adc->bus_i_ma = bus_i_diff_mv * WPT_BUS_I_MA_PER_MV;    /* 换算母线电流 mA */
    adc->coil_i_ma = coil_i_diff_mv * WPT_COIL_I_MA_PER_MV; /* 换算线圈电流 mA */
}

static uint8_t CheckFault(const WptAdc *adc)
{
    if (g_break_fault) /* 硬件 BKIN 故障优先级最高 */
    {
        return 1;
    }

    if (adc->bus_v_mv > WPT_BUS_OVERVOLT_MV) /* 母线过压 */
    {
        return 1;
    }
    if (adc->res_v_mv > WPT_RES_OVERVOLT_MV) /* 谐振端过压 */
    {
        return 1;
    }
    if (adc->bus_i_ma > WPT_BUS_OVERCURRENT_MA) /* 母线过流 */
    {
        return 1;
    }
    if (adc->coil_i_ma > WPT_COIL_OVERCURRENT_MA) /* 线圈过流 */
    {
        return 1;
    }

    if (adc->bus_v_raw > WPT_BUS_V_RAW_MAX) /* 母线电压原始值保护 */
    {
        return 1;
    }
    if (adc->bus_i_raw > WPT_BUS_I_RAW_MAX) /* 母线电流原始值保护 */
    {
        return 1;
    }
    if (adc->coil_i_raw > WPT_COIL_I_RAW_MAX) /* 线圈电流原始值保护 */
    {
        return 1;
    }
    if (adc->res_v_raw > WPT_RES_V_RAW_MAX) /* 谐振电压原始值保护 */
    {
        return 1;
    }
    if (adc->temp_raw > WPT_TEMP_RAW_MAX) /* 温度原始值保护 */
    {
        return 1;
    }

    return 0; /* 没有发现故障 */
}

static void ClearFaults(void)
{
    g_break_fault = 0; /* 清除硬件故障标志 */
}

void HAL_TIMEx_BreakCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM1) /* 确认是 TIM1 触发的故障关断回调 */
    {
        g_break_fault = 1;      /* 记录硬件故障 */
        g_pwm_running = 0;      /* 硬件已经关断输出，软件状态也同步为停止 */
        InverterStop();         /* 再执行一次软件停机，关闭驱动使能 */
        g_state = WPT_STATE_FAULT; /* 进入故障状态 */
    }
}

#if WPT_USE_TIM1_BKIN
void TIM1_BRK_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim1); /* 把 TIM1 故障关断中断交给 HAL 处理 */
}
#endif

/* =========================
 * 8) GPIO、ADC、TIM1 初始化
 * =========================
 */

/* ========================= 引脚配置代码：GPIO 初始化在这里 =========================
 * 如果不用 CubeMX 自动生成 GPIO，这个函数就是本文件的引脚配置入口。
 *
 * 本函数做这些事：
 *   1. 开启 GPIOA、GPIOB、AFIO 时钟。
 *   2. 把 PA8、PA9 配置成 TIM1 复用推挽输出。
 *   3. 把 PB13、PB14 配置成 TIM1 互补 PWM 复用推挽输出。
 *   4. 可选：把 PB12 配置成 TIM1_BKIN 故障输入。
 *   5. 把 PA0 到 PA4 配置成 ADC 模拟输入。
 *   6. 可选：把 PB0 配置成半桥驱动使能输出。
 */
static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct; /* GPIO 初始化结构体 */

    __HAL_RCC_GPIOA_CLK_ENABLE(); /* 开启 GPIOA 时钟，PA0 到 PA9 会用到 */
    __HAL_RCC_GPIOB_CLK_ENABLE(); /* 开启 GPIOB 时钟，PB0、PB12、PB13、PB14 会用到 */
    __HAL_RCC_AFIO_CLK_ENABLE();  /* 开启复用功能时钟，TIM1 输出属于复用功能 */

    GPIO_InitStruct.Pin = WPT_PIN_TIM1_CH1 | WPT_PIN_TIM1_CH2; /* 选择 PA8 和 PA9 */
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;                    /* 复用推挽输出，由 TIM1 控制引脚 */
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;              /* 高频 PWM 输出用高速模式 */
    HAL_GPIO_Init(WPT_GPIO_TIM1_PORT_A, &GPIO_InitStruct);     /* 应用 PA8、PA9 配置 */

    GPIO_InitStruct.Pin = WPT_PIN_TIM1_CH1N | WPT_PIN_TIM1_CH2N; /* 选择 PB13 和 PB14 */
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;                      /* 复用推挽输出，由 TIM1 控制互补输出 */
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;                /* 高频 PWM 输出用高速模式 */
    HAL_GPIO_Init(WPT_GPIO_TIM1_PORT_B, &GPIO_InitStruct);       /* 应用 PB13、PB14 配置 */

#if WPT_USE_TIM1_BKIN
    GPIO_InitStruct.Pin = WPT_PIN_TIM1_BKIN;       /* 选择 PB12 */
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;        /* BKIN 是输入脚 */
    HAL_GPIO_Init(WPT_GPIO_TIM1_PORT_B, &GPIO_InitStruct); /* 应用 PB12 配置 */
#endif

    GPIO_InitStruct.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4; /* 选择 PA0 到 PA4 */
    GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;       /* ADC 引脚必须配置成模拟输入 */
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);        /* 应用 PA0 到 PA4 配置 */

#if WPT_USE_DRIVER_EN
    GPIO_InitStruct.Pin = WPT_DRIVER_EN_PIN;       /* 选择驱动使能脚 PB0 */
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;    /* 普通推挽输出，用来拉高或拉低使能脚 */
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;   /* 使能脚不需要高速 */
    HAL_GPIO_Init(WPT_DRIVER_EN_PORT, &GPIO_InitStruct); /* 应用驱动使能脚配置 */
    DriverEnable(0);                               /* 初始化后先关闭驱动 */
#endif
}

static void MX_ADC1_Init(void)
{
    __HAL_RCC_ADC1_CLK_ENABLE();                    /* 开启 ADC1 外设时钟 */

    hadc1.Instance = ADC1;                          /* 使用 ADC1 */
    hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;     /* 不用扫描模式，代码里逐个通道读取 */
    hadc1.Init.ContinuousConvMode = DISABLE;        /* 不用连续转换，每次手动启动一次 */
    hadc1.Init.DiscontinuousConvMode = DISABLE;     /* 不用间断转换 */
    hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START; /* 软件触发 ADC */
    hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;     /* 采样结果右对齐 */
    hadc1.Init.NbrOfConversion = 1;                 /* 每次只转换一个通道 */

    if (HAL_ADC_Init(&hadc1) != HAL_OK)             /* 初始化 ADC1 */
    {
        Error_Handler();                            /* 初始化失败就停机 */
    }
}

static void MX_TIM1_Init(void)
{
    TIM_OC_InitTypeDef sConfigOC;                         /* PWM 输出通道配置 */
    TIM_BreakDeadTimeConfigTypeDef sBreakDeadTimeConfig;  /* 死区和故障关断配置 */

    __HAL_RCC_TIM1_CLK_ENABLE();                    /* 开启 TIM1 外设时钟 */

    htim1.Instance = TIM1;                          /* 使用 TIM1，高级定时器支持互补 PWM 和死区 */
    htim1.Init.Prescaler = 0;                       /* 不分频，保持最高计数时钟 */
    htim1.Init.CounterMode = TIM_COUNTERMODE_UP;    /* 向上计数模式 */
    htim1.Init.Period = (TIM1_GetClockHz() / WPT_FREQ_START_HZ) - 1UL; /* 初始 PWM 周期 */
    htim1.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1; /* 定时器时钟不再分频 */
    htim1.Init.RepetitionCounter = 0;               /* 每个周期都更新 */
    htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE; /* 开启 ARR 预装载，调频更稳 */

    if (HAL_TIM_PWM_Init(&htim1) != HAL_OK)         /* 初始化 TIM1 为 PWM 模式 */
    {
        Error_Handler();                            /* 初始化失败就停机 */
    }

    sConfigOC.OCMode = TIM_OCMODE_PWM1;             /* CH1 使用 PWM1 模式 */
    sConfigOC.Pulse = htim1.Init.Period / 2UL;      /* CH1 初始 50% 占空比 */
    sConfigOC.OCPolarity = WPT_HIGH_SIDE_ACTIVE_HIGH ? TIM_OCPOLARITY_HIGH : TIM_OCPOLARITY_LOW; /* 主输出极性 */
    sConfigOC.OCNPolarity = WPT_LOW_SIDE_ACTIVE_HIGH ? TIM_OCNPOLARITY_HIGH : TIM_OCNPOLARITY_LOW; /* 互补输出极性 */
    sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;      /* 不使用快速模式 */
    sConfigOC.OCIdleState = TIM_OCIDLESTATE_RESET;  /* 空闲时主输出为低 */
    sConfigOC.OCNIdleState = TIM_OCNIDLESTATE_RESET; /* 空闲时互补输出为低 */

    if (HAL_TIM_PWM_ConfigChannel(&htim1, &sConfigOC, TIM_CHANNEL_1) != HAL_OK) /* 配置 CH1/CH1N */
    {
        Error_Handler();
    }

    sConfigOC.OCMode = TIM_OCMODE_PWM2;             /* CH2 使用 PWM2，和 CH1 形成反相桥臂 */
    if (HAL_TIM_PWM_ConfigChannel(&htim1, &sConfigOC, TIM_CHANNEL_2) != HAL_OK) /* 配置 CH2/CH2N */
    {
        Error_Handler();
    }

    sBreakDeadTimeConfig.OffStateRunMode = TIM_OSSR_ENABLE; /* 运行态关闭输出时保持安全电平 */
    sBreakDeadTimeConfig.OffStateIDLEMode = TIM_OSSI_ENABLE; /* 空闲态关闭输出时保持安全电平 */
    sBreakDeadTimeConfig.LockLevel = TIM_LOCKLEVEL_OFF;      /* 不锁定配置，方便调试 */
    sBreakDeadTimeConfig.DeadTime = TIM1_DeadTimeCodeFromNs(WPT_DEADTIME_NS); /* 设置死区时间 */
    sBreakDeadTimeConfig.BreakState = WPT_USE_TIM1_BKIN ? TIM_BREAK_ENABLE : TIM_BREAK_DISABLE; /* 是否启用硬件关断输入 */
    sBreakDeadTimeConfig.BreakPolarity = WPT_TIM1_BKIN_ACTIVE_HIGH ? TIM_BREAKPOLARITY_HIGH : TIM_BREAKPOLARITY_LOW; /* BKIN 触发极性 */
    sBreakDeadTimeConfig.AutomaticOutput = TIM_AUTOMATICOUTPUT_DISABLE; /* 故障后不自动恢复输出 */

    if (HAL_TIMEx_ConfigBreakDeadTime(&htim1, &sBreakDeadTimeConfig) != HAL_OK) /* 写入死区和故障关断配置 */
    {
        Error_Handler();
    }

#if WPT_USE_TIM1_BKIN
    HAL_NVIC_SetPriority(TIM1_BRK_IRQn, 0, 0); /* BKIN 中断最高优先级 */
    HAL_NVIC_EnableIRQ(TIM1_BRK_IRQn);         /* 使能 TIM1 故障关断中断 */
#endif
}

/* =========================
 * 9) 硬件辅助函数
 * =========================
 */

static uint32_t TIM1_GetClockHz(void)
{
    RCC_ClkInitTypeDef clk;     /* 当前时钟树配置 */
    uint32_t flash_latency;     /* Flash 延时，读取时钟配置时需要 */
    uint32_t pclk2;             /* APB2 外设时钟 */
    uint32_t timclk;            /* TIM1 实际计数时钟 */

    HAL_RCC_GetClockConfig(&clk, &flash_latency); /* 读取当前 RCC 配置 */
    pclk2 = HAL_RCC_GetPCLK2Freq();               /* TIM1 挂在 APB2 上 */

    if (clk.APB2CLKDivider == RCC_HCLK_DIV1)      /* APB2 不分频时 */
    {
        timclk = pclk2;                           /* 定时器时钟等于 PCLK2 */
    }
    else
    {
        timclk = pclk2 * 2UL;                     /* APB 分频不为 1 时，定时器时钟翻倍 */
    }

    return timclk;                                /* 返回 TIM1 实际时钟 */
}

static uint8_t TIM1_DeadTimeCodeFromNs(uint32_t deadtime_ns)
{
    uint32_t timclk = TIM1_GetClockHz();          /* 获取 TIM1 时钟 */
    uint32_t t_dts_ns = 1000000000UL / timclk;    /* 一个定时器计数对应的 ns */
    uint32_t ticks;                               /* 死区需要的计数个数 */

    if (t_dts_ns == 0)                            /* 防止极端情况下除零或计算异常 */
    {
        t_dts_ns = 1;
    }

    ticks = (deadtime_ns + t_dts_ns - 1UL) / t_dts_ns; /* ns 死区向上换算成计数 */

    if (ticks <= 127UL)                           /* BDTR 低范围：直接写入计数 */
    {
        return (uint8_t)ticks;
    }
    if (ticks <= 254UL)                           /* BDTR 中范围：2 倍步进编码 */
    {
        return (uint8_t)(0x80UL | ((ticks / 2UL) - 64UL));
    }
    if (ticks <= 504UL)                           /* BDTR 高范围：8 倍步进编码 */
    {
        return (uint8_t)(0xC0UL | ((ticks / 8UL) - 32UL));
    }
    if (ticks <= 1008UL)                          /* BDTR 最高范围：16 倍步进编码 */
    {
        return (uint8_t)(0xE0UL | ((ticks / 16UL) - 32UL));
    }

    return 0xFF;                                  /* 超出范围时使用最大死区编码 */
}

static void DriverEnable(uint8_t enable)
{
#if WPT_USE_DRIVER_EN
    GPIO_PinState state;                          /* GPIO 输出电平 */

    if (enable)                                   /* 需要使能驱动 */
    {
        state = WPT_DRIVER_EN_ACTIVE_HIGH ? GPIO_PIN_SET : GPIO_PIN_RESET; /* 根据有效电平输出使能态 */
    }
    else                                          /* 需要关闭驱动 */
    {
        state = WPT_DRIVER_EN_ACTIVE_HIGH ? GPIO_PIN_RESET : GPIO_PIN_SET; /* 根据有效电平输出关闭态 */
    }

    HAL_GPIO_WritePin(WPT_DRIVER_EN_PORT, WPT_DRIVER_EN_PIN, state); /* 写驱动使能脚 */
#else
    (void)enable;                                 /* 不使用使能脚时，避免编译器警告 */
#endif
}

/* =========================
 * 10) 时钟配置和错误处理
 * =========================
 */

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct;         /* 振荡器配置结构体 */
    RCC_ClkInitTypeDef RCC_ClkInitStruct;         /* 系统总线时钟配置结构体 */
    RCC_PeriphCLKInitTypeDef PeriphClkInit;       /* 外设时钟配置结构体 */

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE; /* 使用外部高速晶振 HSE */
    RCC_OscInitStruct.HSEState = RCC_HSE_ON;                    /* 打开 HSE */
    RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;     /* HSE 不预分频 */
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;                    /* 同时打开 HSI，作为内部备用时钟 */
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;                /* 打开 PLL */
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;        /* PLL 输入来自 HSE */
    RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;                /* 8MHz HSE * 9 = 72MHz */

    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)        /* 写入振荡器配置 */
    {
        Error_Handler();                                        /* 外部晶振不对会进这里 */
    }

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK |
                                  RCC_CLOCKTYPE_SYSCLK |
                                  RCC_CLOCKTYPE_PCLK1 |
                                  RCC_CLOCKTYPE_PCLK2;          /* 同时配置核心和总线时钟 */
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;   /* 系统时钟来自 PLL */
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;          /* AHB = 72MHz */
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;           /* APB1 = 36MHz，符合 F103 限制 */
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;           /* APB2 = 72MHz，TIM1 在 APB2 */

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK) /* 写入总线时钟配置 */
    {
        Error_Handler();
    }

    PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_ADC;     /* 配置 ADC 时钟 */
    PeriphClkInit.AdcClockSelection = RCC_ADCPCLK2_DIV6;        /* ADC 时钟 = 72MHz / 6 = 12MHz */

    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)    /* 写入 ADC 时钟配置 */
    {
        Error_Handler();
    }
}

static void Error_Handler(void)
{
    if (htim1.Instance == TIM1) /* 如果 TIM1 已经初始化，就先关闭逆变输出 */
    {
        InverterStop();
    }
    __disable_irq();           /* 关闭中断，避免故障后继续执行控制逻辑 */

    while (1)
    {
        /* 程序停在这里。实际比赛板可以在这里加 LED 闪烁，用来提示初始化或运行错误。 */
    }
}
