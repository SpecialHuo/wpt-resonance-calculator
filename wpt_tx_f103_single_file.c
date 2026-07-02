/*
 * STM32F103 wireless charging transmitter single-file template
 *
 * Hardware idea:
 *   STM32F103 TIM1 -> two half-bridge drivers -> full bridge -> LCC network -> TX coil
 *
 * Important:
 *   1. This file is a HAL-based main.c template. It still needs normal STM32 startup,
 *      linker script, HAL library, and stm32f1xx_hal_conf.h from CubeMX/Keil/STM32CubeIDE.
 *   2. Do not test the power stage at high bus voltage first. Start with a current-limited
 *      supply and a low voltage such as 12 V.
 *   3. Software protection is not enough. Connect hardware over-current to TIM1_BKIN
 *      or the driver shutdown pin whenever possible.
 */

#include "stm32f1xx_hal.h"

#ifndef __HAL_TIM_MOE_ENABLE
#define __HAL_TIM_MOE_ENABLE(__HANDLE__)  ((__HANDLE__)->Instance->BDTR |= TIM_BDTR_MOE)
#endif

#ifndef __HAL_TIM_MOE_DISABLE
#define __HAL_TIM_MOE_DISABLE(__HANDLE__) ((__HANDLE__)->Instance->BDTR &= ~TIM_BDTR_MOE)
#endif

/* =========================
 * 1) Competition quick macros
 * =========================
 * Put all unknown field parameters here. At the competition, change these first.
 */

/* -------- Power stage frequency --------
 * Full bridge drives the LCC by 50% duty square wave.
 * Power is adjusted mainly by changing frequency.
 */
#define WPT_FREQ_START_HZ          180000UL  /* Start far away from resonance if unknown. */
#define WPT_FREQ_MIN_HZ             80000UL  /* Lowest allowed switching frequency. */
#define WPT_FREQ_MAX_HZ            250000UL  /* Highest allowed switching frequency. */
#define WPT_FREQ_STEP_HZ              500UL  /* Frequency step during simple control. */
#define WPT_SWEEP_STEP_HZ            1000UL  /* Step used during receiver/load search. */

/* If you do not know the safe side of resonance, use sweep mode first at low bus voltage.
 * For many LCC systems, moving closer to resonance increases current, but the exact side
 * depends on your compensation values. This macro defines the first assumption.
 */
#define WPT_POWER_UP_BY_LOWER_FREQ       1   /* 1: lower freq increases power; 0: higher freq increases power. */

/* ========================= 引脚配置宏：先改这里 =========================
 * 下面这些宏只是“引脚编号定义”，真正把引脚配置成复用输出/模拟输入的代码
 * 在后面的 MX_GPIO_Init() 函数里。
 */

/* -------- TIM1 PWM output pins, default STM32F103 common mapping --------
 * TIM1_CH1  PA8   -> half bridge A high-side input
 * TIM1_CH1N PB13  -> half bridge A low-side input
 * TIM1_CH2  PA9   -> half bridge B high-side input
 * TIM1_CH2N PB14  -> half bridge B low-side input
 * TIM1_BKIN PB12  -> external hardware fault input
 */
#define WPT_GPIO_TIM1_PORT_A       GPIOA
#define WPT_GPIO_TIM1_PORT_B       GPIOB
#define WPT_PIN_TIM1_CH1           GPIO_PIN_8
#define WPT_PIN_TIM1_CH2           GPIO_PIN_9
#define WPT_PIN_TIM1_CH1N          GPIO_PIN_13
#define WPT_PIN_TIM1_CH2N          GPIO_PIN_14
#define WPT_PIN_TIM1_BKIN          GPIO_PIN_12

/* TIM1_BKIN hardware shutdown.
 * Keep this 0 if PB12 is not connected to a comparator/fault circuit with a
 * defined inactive level. A floating BKIN pin can make the inverter never start.
 */
#define WPT_USE_TIM1_BKIN          0
#define WPT_TIM1_BKIN_ACTIVE_HIGH  1

/* -------- Driver enable pin --------
 * Optional driver enable pin. If your driver has no EN/SD pin, set WPT_USE_DRIVER_EN to 0.
 */
#define WPT_USE_DRIVER_EN          1
#define WPT_DRIVER_EN_PORT         GPIOB
#define WPT_DRIVER_EN_PIN          GPIO_PIN_0
#define WPT_DRIVER_EN_ACTIVE_HIGH  1

/* Most half-bridge drivers use active-high HIN/LIN inputs. If your driver input
 * is active-low or has an inverting stage, change these two macros.
 */
#define WPT_HIGH_SIDE_ACTIVE_HIGH  1
#define WPT_LOW_SIDE_ACTIVE_HIGH   1

/* -------- Dead time --------
 * Too small: MOSFET shoot-through.
 * Too large: more loss and waveform distortion.
 * Typical first try: 300 ns to 800 ns, depending on MOSFET/driver.
 */
#define WPT_DEADTIME_NS            500UL

/* -------- ADC input pins / channels --------
 * Default:
 *   PA0 ADC1_IN0: DC bus voltage
 *   PA1 ADC1_IN1: DC bus current
 *   PA2 ADC1_IN2: resonant/coil current
 *   PA3 ADC1_IN3: resonant capacitor or coil voltage
 *   PA4 ADC1_IN4: temperature sensor divider/NTC voltage
 */
#define WPT_ADC_BUS_V_CH           ADC_CHANNEL_0
#define WPT_ADC_BUS_I_CH           ADC_CHANNEL_1
#define WPT_ADC_COIL_I_CH          ADC_CHANNEL_2
#define WPT_ADC_RES_V_CH           ADC_CHANNEL_3
#define WPT_ADC_TEMP_CH            ADC_CHANNEL_4

/* ADC conversion constants.
 * If you do not know the scale yet, leave these as rough values and debug by printing raw ADC.
 */
#define WPT_ADC_VREF_MV            3300UL
#define WPT_ADC_MAX_COUNT          4095UL

/* Voltage divider:
 * Real voltage = ADC voltage * (R_HIGH + R_LOW) / R_LOW.
 * Example: 100k high, 4.7k low means 48 V -> about 2.15 V ADC.
 */
#define WPT_BUS_V_R_HIGH_OHM       100000UL
#define WPT_BUS_V_R_LOW_OHM          4700UL
#define WPT_RES_V_R_HIGH_OHM       100000UL
#define WPT_RES_V_R_LOW_OHM          4700UL

/* Current sensor:
 * Real current in mA = ADC voltage in mV * CURRENT_MA_PER_MV.
 * For ACS712-20A, set the zero-point macros below; for shunt amplifier it is usually direct.
 * For first bring-up, you can set conservative raw limits below instead.
 */
#define WPT_BUS_I_MA_PER_MV        10UL
#define WPT_COIL_I_MA_PER_MV       10UL

/* Current sensor zero point. For shunt amplifier with 0 A = 0 V, use 0.
 * For Hall sensors such as ACS712 powered by 3.3 V, use about 1650 mV.
 */
#define WPT_BUS_I_ZERO_MV          0UL
#define WPT_COIL_I_ZERO_MV         0UL

/* -------- Protection thresholds --------
 * These values must be conservative before the real hardware is measured.
 * The raw thresholds are kept too, because in a competition you may only have ADC counts.
 */
#define WPT_BUS_OVERVOLT_MV        60000UL
#define WPT_RES_OVERVOLT_MV        90000UL
#define WPT_BUS_OVERCURRENT_MA      5000UL
#define WPT_COIL_OVERCURRENT_MA     8000UL

/* If ADC scale is not trusted, raw thresholds still protect roughly.
 * 4095 means disabled by raw value. Lower it during first test if needed.
 */
#define WPT_BUS_V_RAW_MAX          4095UL
#define WPT_BUS_I_RAW_MAX          4095UL
#define WPT_COIL_I_RAW_MAX         4095UL
#define WPT_RES_V_RAW_MAX          4095UL
#define WPT_TEMP_RAW_MAX           4095UL

/* -------- Control target --------
 * If receiver communication is not available, target coil/bus current is a practical first target.
 */
#define WPT_TARGET_COIL_CURRENT_MA 3000UL
#define WPT_TARGET_HYST_MA          200UL

/* -------- Time settings -------- */
#define WPT_CONTROL_PERIOD_MS      5UL       /* Main control loop period. */
#define WPT_SOFTSTART_PERIOD_MS    20UL      /* Frequency update period during soft start. */
#define WPT_FAULT_RESTART_MS       0UL       /* 0: latch fault until reset. Non-zero: auto retry delay. */

/* -------- Startup strategy --------
 * 0: start directly at WPT_FREQ_START_HZ, then closed-loop control.
 * 1: sweep from WPT_FREQ_MAX_HZ down to WPT_FREQ_MIN_HZ while watching current.
 */
#define WPT_USE_STARTUP_SWEEP      1
#define WPT_SWEEP_CURRENT_MA       500UL     /* Receiver/load detected when coil current exceeds this. */

/* =========================
 * 2) Global HAL handles
 * =========================
 */

TIM_HandleTypeDef htim1;
ADC_HandleTypeDef hadc1;

typedef enum
{
    WPT_STATE_IDLE = 0,
    WPT_STATE_SWEEP,
    WPT_STATE_SOFTSTART,
    WPT_STATE_RUN,
    WPT_STATE_FAULT
} WptState;

typedef struct
{
    uint16_t bus_v_raw;
    uint16_t bus_i_raw;
    uint16_t coil_i_raw;
    uint16_t res_v_raw;
    uint16_t temp_raw;

    uint32_t bus_v_mv;
    uint32_t bus_i_ma;
    uint32_t coil_i_ma;
    uint32_t res_v_mv;
} WptAdc;

static volatile WptState g_state = WPT_STATE_IDLE;
static volatile uint8_t g_break_fault = 0;
static volatile uint8_t g_pwm_running = 0;
static uint32_t g_freq_hz = WPT_FREQ_START_HZ;
static uint32_t g_last_control_ms = 0;
static uint32_t g_last_softstart_ms = 0;
static uint32_t g_fault_ms = 0;
static WptAdc g_adc;

/* =========================
 * 3) Forward declarations
 * =========================
 */

void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_ADC1_Init(void);
static void MX_TIM1_Init(void);

static uint8_t TIM1_DeadTimeCodeFromNs(uint32_t deadtime_ns);
static uint32_t TIM1_GetClockHz(void);
static void DriverEnable(uint8_t enable);
static void InverterStart(void);
static void InverterStop(void);
static void InverterSetFrequency(uint32_t freq_hz);
static uint16_t ADC_ReadChannel(uint32_t channel);
static uint32_t ADC_RawToMv(uint16_t raw);
static uint32_t ScaleDividerMv(uint32_t adc_mv, uint32_t r_high, uint32_t r_low);
static void ReadAllAdc(WptAdc *adc);
static uint8_t CheckFault(const WptAdc *adc);
static void ClearFaults(void);
static void ControlTask(void);
static void SweepTask(void);
static void SoftStartTask(void);
static void RunTask(void);
static void Error_Handler(void);

/* =========================
 * 4) Main
 * =========================
 */

int main(void)
{
    HAL_Init();
    SystemClock_Config();

    MX_GPIO_Init();
    MX_ADC1_Init();
    MX_TIM1_Init();

    /* F1 ADC needs calibration for better readings. */
    HAL_ADCEx_Calibration_Start(&hadc1);

    DriverEnable(0);
    InverterStop();
    ClearFaults();

    /* Competition version: start automatically after power-on.
     * If you have a start button, change this to wait for the button.
     */
#if WPT_USE_STARTUP_SWEEP
    g_state = WPT_STATE_SWEEP;
    g_freq_hz = WPT_FREQ_MAX_HZ;
#else
    g_state = WPT_STATE_SOFTSTART;
    g_freq_hz = WPT_FREQ_START_HZ;
#endif

    while (1)
    {
        ReadAllAdc(&g_adc);

        /* Protection is checked every loop. Do not wait for slow control timing. */
        if (CheckFault(&g_adc))
        {
            InverterStop();
            g_fault_ms = HAL_GetTick();
            g_state = WPT_STATE_FAULT;
        }

        switch (g_state)
        {
        case WPT_STATE_IDLE:
            InverterStop();
            break;

        case WPT_STATE_SWEEP:
            SweepTask();
            break;

        case WPT_STATE_SOFTSTART:
            SoftStartTask();
            break;

        case WPT_STATE_RUN:
            ControlTask();
            break;

        case WPT_STATE_FAULT:
            InverterStop();
#if WPT_FAULT_RESTART_MS > 0
            if ((HAL_GetTick() - g_fault_ms) > WPT_FAULT_RESTART_MS)
            {
                ClearFaults();
                g_state = WPT_STATE_SWEEP;
                g_freq_hz = WPT_FREQ_MAX_HZ;
            }
#endif
            break;

        default:
            g_state = WPT_STATE_FAULT;
            break;
        }
    }
}

/* =========================
 * 5) Full bridge PWM
 * =========================
 */

static void InverterStart(void)
{
    if (g_pwm_running)
    {
        return;
    }

    /* CH1/CH1N controls one half bridge, CH2/CH2N controls the other.
     * CH1 uses PWM1 and CH2 uses PWM2, both at 50%, so the two bridge legs
     * are 180 degrees apart. The load sees a square wave.
     *
     * Do not enable the external driver until all four timer outputs are armed.
     * This avoids a short interval where only one bridge leg is switching.
     */
    DriverEnable(0);

    __HAL_TIM_ENABLE(&htim1);
    htim1.Instance->CCER |= TIM_CCER_CC1E | TIM_CCER_CC1NE |
                            TIM_CCER_CC2E | TIM_CCER_CC2NE;
    __HAL_TIM_MOE_ENABLE(&htim1);

    DriverEnable(1);
    g_pwm_running = 1;
}

static void InverterStop(void)
{
    /* Turn off the external driver first, then remove timer outputs. */
    DriverEnable(0);

    if (htim1.Instance == TIM1)
    {
        __HAL_TIM_MOE_DISABLE(&htim1);
        htim1.Instance->CCER &= ~(TIM_CCER_CC1E | TIM_CCER_CC1NE |
                                  TIM_CCER_CC2E | TIM_CCER_CC2NE);
        __HAL_TIM_DISABLE(&htim1);
    }

    g_pwm_running = 0;
}

static void InverterSetFrequency(uint32_t freq_hz)
{
    uint32_t timer_clk;
    uint32_t arr;

    if (freq_hz < WPT_FREQ_MIN_HZ)
    {
        freq_hz = WPT_FREQ_MIN_HZ;
    }
    if (freq_hz > WPT_FREQ_MAX_HZ)
    {
        freq_hz = WPT_FREQ_MAX_HZ;
    }

    timer_clk = TIM1_GetClockHz();
    arr = (timer_clk / freq_hz) - 1UL;
    if (arr < 10UL)
    {
        arr = 10UL;
    }

    /* Update frequency and keep both legs at 50% duty.
     * ARR is the period. CCR = ARR / 2 creates 50% duty.
     * ARR preload is enabled, so the change takes effect on update event.
     * We do not stop PWM here, because repeated stop/start can disturb the bridge.
     */
    __HAL_TIM_SET_AUTORELOAD(&htim1, arr);
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, arr / 2UL);
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, arr / 2UL);
    htim1.Instance->EGR = TIM_EGR_UG;

    g_freq_hz = freq_hz;
}

/* =========================
 * 6) State machine tasks
 * =========================
 */

static void SweepTask(void)
{
    uint32_t now = HAL_GetTick();

    if ((now - g_last_softstart_ms) < WPT_SOFTSTART_PERIOD_MS)
    {
        return;
    }
    g_last_softstart_ms = now;

    InverterSetFrequency(g_freq_hz);
    InverterStart();

    /* Sweep searches for a receiver/load by watching coil current.
     * Keep bus voltage low the first time, because sweeping can hit resonance.
     */
    if (g_adc.coil_i_ma > WPT_SWEEP_CURRENT_MA)
    {
        g_state = WPT_STATE_RUN;
        return;
    }

    if (g_freq_hz > (WPT_FREQ_MIN_HZ + WPT_SWEEP_STEP_HZ))
    {
        g_freq_hz -= WPT_SWEEP_STEP_HZ;
    }
    else
    {
        /* No load found. Go back to high frequency and keep searching gently. */
        g_freq_hz = WPT_FREQ_MAX_HZ;
    }
}

static void SoftStartTask(void)
{
    uint32_t now = HAL_GetTick();

    if ((now - g_last_softstart_ms) < WPT_SOFTSTART_PERIOD_MS)
    {
        return;
    }
    g_last_softstart_ms = now;

    /* Soft start begins at the configured start frequency.
     * It then lets the normal control loop take over.
     */
    InverterSetFrequency(g_freq_hz);
    InverterStart();
    g_state = WPT_STATE_RUN;
}

static void ControlTask(void)
{
    uint32_t now = HAL_GetTick();

    if ((now - g_last_control_ms) < WPT_CONTROL_PERIOD_MS)
    {
        return;
    }
    g_last_control_ms = now;

    RunTask();
}

static void RunTask(void)
{
    uint32_t target_low = WPT_TARGET_COIL_CURRENT_MA - WPT_TARGET_HYST_MA;
    uint32_t target_high = WPT_TARGET_COIL_CURRENT_MA + WPT_TARGET_HYST_MA;

    /* Very simple competition control:
     *   - below target: increase power
     *   - above target: decrease power
     *
     * For a real product, replace this with a PI loop and receiver communication.
     */
    if (g_adc.coil_i_ma < target_low)
    {
#if WPT_POWER_UP_BY_LOWER_FREQ
        if (g_freq_hz > (WPT_FREQ_MIN_HZ + WPT_FREQ_STEP_HZ))
        {
            g_freq_hz -= WPT_FREQ_STEP_HZ;
        }
#else
        if (g_freq_hz < (WPT_FREQ_MAX_HZ - WPT_FREQ_STEP_HZ))
        {
            g_freq_hz += WPT_FREQ_STEP_HZ;
        }
#endif
    }
    else if (g_adc.coil_i_ma > target_high)
    {
#if WPT_POWER_UP_BY_LOWER_FREQ
        if (g_freq_hz < (WPT_FREQ_MAX_HZ - WPT_FREQ_STEP_HZ))
        {
            g_freq_hz += WPT_FREQ_STEP_HZ;
        }
#else
        if (g_freq_hz > (WPT_FREQ_MIN_HZ + WPT_FREQ_STEP_HZ))
        {
            g_freq_hz -= WPT_FREQ_STEP_HZ;
        }
#endif
    }

    InverterSetFrequency(g_freq_hz);
    InverterStart();
}

/* =========================
 * 7) ADC and protection
 * =========================
 */

static uint16_t ADC_ReadChannel(uint32_t channel)
{
    ADC_ChannelConfTypeDef sConfig;
    uint16_t raw = 0;

    sConfig.Channel = channel;
    sConfig.Rank = ADC_REGULAR_RANK_1;
    sConfig.SamplingTime = ADC_SAMPLETIME_55CYCLES_5;

    if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
    {
        return 0;
    }

    HAL_ADC_Start(&hadc1);
    if (HAL_ADC_PollForConversion(&hadc1, 2) == HAL_OK)
    {
        raw = (uint16_t)HAL_ADC_GetValue(&hadc1);
    }
    HAL_ADC_Stop(&hadc1);

    return raw;
}

static uint32_t ADC_RawToMv(uint16_t raw)
{
    return ((uint32_t)raw * WPT_ADC_VREF_MV) / WPT_ADC_MAX_COUNT;
}

static uint32_t ScaleDividerMv(uint32_t adc_mv, uint32_t r_high, uint32_t r_low)
{
    return (adc_mv * (r_high + r_low)) / r_low;
}

static void ReadAllAdc(WptAdc *adc)
{
    uint32_t bus_v_adc_mv;
    uint32_t bus_i_adc_mv;
    uint32_t coil_i_adc_mv;
    uint32_t res_v_adc_mv;
    uint32_t bus_i_diff_mv;
    uint32_t coil_i_diff_mv;

    adc->bus_v_raw = ADC_ReadChannel(WPT_ADC_BUS_V_CH);
    adc->bus_i_raw = ADC_ReadChannel(WPT_ADC_BUS_I_CH);
    adc->coil_i_raw = ADC_ReadChannel(WPT_ADC_COIL_I_CH);
    adc->res_v_raw = ADC_ReadChannel(WPT_ADC_RES_V_CH);
    adc->temp_raw = ADC_ReadChannel(WPT_ADC_TEMP_CH);

    bus_v_adc_mv = ADC_RawToMv(adc->bus_v_raw);
    bus_i_adc_mv = ADC_RawToMv(adc->bus_i_raw);
    coil_i_adc_mv = ADC_RawToMv(adc->coil_i_raw);
    res_v_adc_mv = ADC_RawToMv(adc->res_v_raw);

    adc->bus_v_mv = ScaleDividerMv(bus_v_adc_mv, WPT_BUS_V_R_HIGH_OHM, WPT_BUS_V_R_LOW_OHM);
    adc->res_v_mv = ScaleDividerMv(res_v_adc_mv, WPT_RES_V_R_HIGH_OHM, WPT_RES_V_R_LOW_OHM);

    /* Current sensors may have a mid-supply zero point. Use absolute distance
     * from zero so both current directions are protected.
     */
    bus_i_diff_mv = (bus_i_adc_mv > WPT_BUS_I_ZERO_MV)
                    ? (bus_i_adc_mv - WPT_BUS_I_ZERO_MV)
                    : (WPT_BUS_I_ZERO_MV - bus_i_adc_mv);
    coil_i_diff_mv = (coil_i_adc_mv > WPT_COIL_I_ZERO_MV)
                     ? (coil_i_adc_mv - WPT_COIL_I_ZERO_MV)
                     : (WPT_COIL_I_ZERO_MV - coil_i_adc_mv);

    adc->bus_i_ma = bus_i_diff_mv * WPT_BUS_I_MA_PER_MV;
    adc->coil_i_ma = coil_i_diff_mv * WPT_COIL_I_MA_PER_MV;
}

static uint8_t CheckFault(const WptAdc *adc)
{
    if (g_break_fault)
    {
        return 1;
    }

    if (adc->bus_v_mv > WPT_BUS_OVERVOLT_MV)
    {
        return 1;
    }
    if (adc->res_v_mv > WPT_RES_OVERVOLT_MV)
    {
        return 1;
    }
    if (adc->bus_i_ma > WPT_BUS_OVERCURRENT_MA)
    {
        return 1;
    }
    if (adc->coil_i_ma > WPT_COIL_OVERCURRENT_MA)
    {
        return 1;
    }

    /* Raw-value protection is useful before sensor scaling is calibrated. */
    if (adc->bus_v_raw > WPT_BUS_V_RAW_MAX)
    {
        return 1;
    }
    if (adc->bus_i_raw > WPT_BUS_I_RAW_MAX)
    {
        return 1;
    }
    if (adc->coil_i_raw > WPT_COIL_I_RAW_MAX)
    {
        return 1;
    }
    if (adc->res_v_raw > WPT_RES_V_RAW_MAX)
    {
        return 1;
    }
    if (adc->temp_raw > WPT_TEMP_RAW_MAX)
    {
        return 1;
    }

    return 0;
}

static void ClearFaults(void)
{
    g_break_fault = 0;
}

/* TIM break callback:
 * If TIM1_BKIN receives a fault signal, HAL calls this callback.
 * The PWM outputs are disabled by timer hardware faster than normal code can react.
 */
void HAL_TIMEx_BreakCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM1)
    {
        g_break_fault = 1;
        g_pwm_running = 0;
        InverterStop();
        g_state = WPT_STATE_FAULT;
    }
}

#if WPT_USE_TIM1_BKIN
void TIM1_BRK_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim1);
}
#endif

/* =========================
 * 8) GPIO, ADC, TIM init
 * =========================
 */

/* ========================= 引脚配置代码：GPIO 初始化在这里 =========================
 * 如果你不用 CubeMX 自动生成 GPIO，这个函数就是本文件的引脚配置入口。
 *
 * 本函数做了这些事：
 *   1. 开启 GPIOA/GPIOB/AFIO 时钟
 *   2. 把 PA8/PA9 配置成 TIM1 复用推挽输出
 *   3. 把 PB13/PB14 配置成 TIM1 互补 PWM 复用推挽输出
 *   4. 可选：把 PB12 配置成 TIM1_BKIN 故障输入
 *   5. 把 PA0~PA4 配置成 ADC 模拟输入
 *   6. 可选：把 PB0 配置成半桥驱动 EN 输出
 */
static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;

    /* 引脚配置 1：开启 GPIO 和复用功能时钟。
     * 不开时钟，后面的 HAL_GPIO_Init() 不会真正生效。
     */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_AFIO_CLK_ENABLE();

    /* 引脚配置 2：TIM1 主 PWM 输出脚。
     * PA8  = TIM1_CH1  -> 半桥 A 高边输入
     * PA9  = TIM1_CH2  -> 半桥 B 高边输入
     * GPIO_MODE_AF_PP 表示复用推挽输出，由 TIM1 外设接管输出波形。
     */
    GPIO_InitStruct.Pin = WPT_PIN_TIM1_CH1 | WPT_PIN_TIM1_CH2;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(WPT_GPIO_TIM1_PORT_A, &GPIO_InitStruct);

    /* 引脚配置 3：TIM1 互补 PWM 输出脚。
     * PB13 = TIM1_CH1N -> 半桥 A 低边输入
     * PB14 = TIM1_CH2N -> 半桥 B 低边输入
     */
    GPIO_InitStruct.Pin = WPT_PIN_TIM1_CH1N | WPT_PIN_TIM1_CH2N;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(WPT_GPIO_TIM1_PORT_B, &GPIO_InitStruct);

#if WPT_USE_TIM1_BKIN
    /* 引脚配置 4：TIM1_BKIN 硬件故障输入脚。
     * PB12 = TIM1_BKIN。
     * 外部过流比较器可以接到这里，一旦触发，TIM1 硬件会关闭 PWM。
     *
     * STM32F1 HAL GPIO_InitTypeDef commonly has no Pull field, so use an
     * external resistor on the PCB/module to define the inactive level.
     */
    GPIO_InitStruct.Pin = WPT_PIN_TIM1_BKIN;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    HAL_GPIO_Init(WPT_GPIO_TIM1_PORT_B, &GPIO_InitStruct);
#endif

    /* 引脚配置 5：ADC 模拟输入脚。
     * PA0 = ADC1_IN0 -> 母线电压
     * PA1 = ADC1_IN1 -> 母线电流
     * PA2 = ADC1_IN2 -> 线圈/谐振电流
     * PA3 = ADC1_IN3 -> 谐振电压
     * PA4 = ADC1_IN4 -> 温度
     */
    GPIO_InitStruct.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4;
    GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

#if WPT_USE_DRIVER_EN
    /* 引脚配置 6：半桥驱动使能脚。
     * PB0 -> driver EN/SD。
     * 如果驱动芯片没有 EN/SD 引脚，把 WPT_USE_DRIVER_EN 改成 0。
     */
    GPIO_InitStruct.Pin = WPT_DRIVER_EN_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(WPT_DRIVER_EN_PORT, &GPIO_InitStruct);
    DriverEnable(0);
#endif
}

static void MX_ADC1_Init(void)
{
    __HAL_RCC_ADC1_CLK_ENABLE();

    hadc1.Instance = ADC1;
    hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
    hadc1.Init.ContinuousConvMode = DISABLE;
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
    hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
    hadc1.Init.NbrOfConversion = 1;

    if (HAL_ADC_Init(&hadc1) != HAL_OK)
    {
        Error_Handler();
    }
}

static void MX_TIM1_Init(void)
{
    TIM_OC_InitTypeDef sConfigOC;
    TIM_BreakDeadTimeConfigTypeDef sBreakDeadTimeConfig;

    __HAL_RCC_TIM1_CLK_ENABLE();

    htim1.Instance = TIM1;
    htim1.Init.Prescaler = 0;
    htim1.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim1.Init.Period = (TIM1_GetClockHz() / WPT_FREQ_START_HZ) - 1UL;
    htim1.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim1.Init.RepetitionCounter = 0;
    htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;

    if (HAL_TIM_PWM_Init(&htim1) != HAL_OK)
    {
        Error_Handler();
    }

    /* Channel 1: PWM mode 1, 50% duty. */
    sConfigOC.OCMode = TIM_OCMODE_PWM1;
    sConfigOC.Pulse = htim1.Init.Period / 2UL;
    sConfigOC.OCPolarity = WPT_HIGH_SIDE_ACTIVE_HIGH ? TIM_OCPOLARITY_HIGH : TIM_OCPOLARITY_LOW;
    sConfigOC.OCNPolarity = WPT_LOW_SIDE_ACTIVE_HIGH ? TIM_OCNPOLARITY_HIGH : TIM_OCNPOLARITY_LOW;
    sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
    sConfigOC.OCIdleState = TIM_OCIDLESTATE_RESET;
    sConfigOC.OCNIdleState = TIM_OCNIDLESTATE_RESET;

    if (HAL_TIM_PWM_ConfigChannel(&htim1, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
    {
        Error_Handler();
    }

    /* Channel 2: PWM mode 2 creates the opposite phase from channel 1. */
    sConfigOC.OCMode = TIM_OCMODE_PWM2;
    if (HAL_TIM_PWM_ConfigChannel(&htim1, &sConfigOC, TIM_CHANNEL_2) != HAL_OK)
    {
        Error_Handler();
    }

    /* Break/dead-time configuration.
     * Enable BKIN only when an external fault circuit is really connected.
     */
    sBreakDeadTimeConfig.OffStateRunMode = TIM_OSSR_ENABLE;
    sBreakDeadTimeConfig.OffStateIDLEMode = TIM_OSSI_ENABLE;
    sBreakDeadTimeConfig.LockLevel = TIM_LOCKLEVEL_OFF;
    sBreakDeadTimeConfig.DeadTime = TIM1_DeadTimeCodeFromNs(WPT_DEADTIME_NS);
    sBreakDeadTimeConfig.BreakState = WPT_USE_TIM1_BKIN ? TIM_BREAK_ENABLE : TIM_BREAK_DISABLE;
    sBreakDeadTimeConfig.BreakPolarity = WPT_TIM1_BKIN_ACTIVE_HIGH ? TIM_BREAKPOLARITY_HIGH : TIM_BREAKPOLARITY_LOW;
    sBreakDeadTimeConfig.AutomaticOutput = TIM_AUTOMATICOUTPUT_DISABLE;

    if (HAL_TIMEx_ConfigBreakDeadTime(&htim1, &sBreakDeadTimeConfig) != HAL_OK)
    {
        Error_Handler();
    }

#if WPT_USE_TIM1_BKIN
    HAL_NVIC_SetPriority(TIM1_BRK_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(TIM1_BRK_IRQn);
#endif
}

/* =========================
 * 9) Small hardware helpers
 * =========================
 */

static uint32_t TIM1_GetClockHz(void)
{
    RCC_ClkInitTypeDef clk;
    uint32_t flash_latency;
    uint32_t pclk2;
    uint32_t timclk;

    HAL_RCC_GetClockConfig(&clk, &flash_latency);
    pclk2 = HAL_RCC_GetPCLK2Freq();

    /* On STM32F1, if APB prescaler is not 1, timer clock is doubled. */
    if (clk.APB2CLKDivider == RCC_HCLK_DIV1)
    {
        timclk = pclk2;
    }
    else
    {
        timclk = pclk2 * 2UL;
    }

    return timclk;
}

static uint8_t TIM1_DeadTimeCodeFromNs(uint32_t deadtime_ns)
{
    uint32_t timclk = TIM1_GetClockHz();
    uint32_t t_dts_ns = 1000000000UL / timclk;
    uint32_t ticks;

    if (t_dts_ns == 0)
    {
        t_dts_ns = 1;
    }

    ticks = (deadtime_ns + t_dts_ns - 1UL) / t_dts_ns;

    /* STM32F1 BDTR deadtime encoding:
     * 0..127: direct ticks
     * 128..254: encoded ranges with coarser resolution.
     */
    if (ticks <= 127UL)
    {
        return (uint8_t)ticks;
    }
    if (ticks <= 254UL)
    {
        return (uint8_t)(0x80UL | ((ticks / 2UL) - 64UL));
    }
    if (ticks <= 504UL)
    {
        return (uint8_t)(0xC0UL | ((ticks / 8UL) - 32UL));
    }
    if (ticks <= 1008UL)
    {
        return (uint8_t)(0xE0UL | ((ticks / 16UL) - 32UL));
    }

    return 0xFF;
}

static void DriverEnable(uint8_t enable)
{
#if WPT_USE_DRIVER_EN
    GPIO_PinState state;

    if (enable)
    {
        state = WPT_DRIVER_EN_ACTIVE_HIGH ? GPIO_PIN_SET : GPIO_PIN_RESET;
    }
    else
    {
        state = WPT_DRIVER_EN_ACTIVE_HIGH ? GPIO_PIN_RESET : GPIO_PIN_SET;
    }

    HAL_GPIO_WritePin(WPT_DRIVER_EN_PORT, WPT_DRIVER_EN_PIN, state);
#else
    (void)enable;
#endif
}

/* =========================
 * 10) Clock and error handler
 * =========================
 */

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct;
    RCC_ClkInitTypeDef RCC_ClkInitStruct;
    RCC_PeriphCLKInitTypeDef PeriphClkInit;

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState = RCC_HSE_ON;
    RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;

    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        Error_Handler();
    }

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK |
                                  RCC_CLOCKTYPE_SYSCLK |
                                  RCC_CLOCKTYPE_PCLK1 |
                                  RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
    {
        Error_Handler();
    }

    PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_ADC;
    PeriphClkInit.AdcClockSelection = RCC_ADCPCLK2_DIV6; /* 72 MHz / 6 = 12 MHz ADC clock. */

    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
    {
        Error_Handler();
    }
}

static void Error_Handler(void)
{
    if (htim1.Instance == TIM1)
    {
        InverterStop();
    }
    __disable_irq();

    while (1)
    {
        /* Stay here. Add LED blink if your board has one. */
    }
}
