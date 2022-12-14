#include "xil_io.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "platform.h"
#include "xparameters.h"
#include "xgpio.h"
#include "xtmrctr.h" //contiene a XTmrCtr_IsExpired (Controlador del timer)
#include "xscugic.h"
#include "xil_exception.h"
#include "xil_printf.h"
#include "xtime_l.h"
#include "xsysmon.h"
#include "xstatus.h"
#include "sleep.h"
#include "sdCard.h"
#include "demo.h"

#include "audio/audio.h"
#include "dma/dma.h"
#include "intc/intc.h"
#include "userio/userio.h"
#include "iic/iic.h"

#include "xaxidma.h"
#include "xdebug.h"
#include "xiic.h"
#include "xaxidma.h"
#ifdef XPAR_INTC_0_DEVICE_ID
#include "xintc.h"
#include "microblaze_sleep.h"
#else
    #include "xscugic.h"
#include "sleep.h"
#include "xil_cache.h"
#endif

#include <string.h>

/************************** Constant Definitions *****************************/

/*
 * Device hardware build related constants.
 */

// Audio constants
// Number of seconds to record/playback
#define NR_SEC_TO_REC_PLAY 5

// ADC/DAC sampling rate in Hz
// #define AUDIO_SAMPLING_RATE		1000
#define AUDIO_SAMPLING_RATE 96000

// Number of samples to record/playback
#define NR_AUDIO_SAMPLES (NR_SEC_TO_REC_PLAY * AUDIO_SAMPLING_RATE)

/* Timeout loop counter for reset
 */
#define RESET_TIMEOUT_COUNTER 10000

#define TEST_START_VALUE 0x0

/************************** Function Prototypes ******************************/
#if (!defined(DEBUG))
extern void xil_printf(const char *format, ...);
#endif

/************************** Variable Definitions *****************************/
/*
 * Device instance definitions
 */

static XIic sIic;
static XAxiDma sAxiDma; /* Instance of the XAxiDma */

static XGpio sUserIO;

#ifdef XPAR_INTC_0_DEVICE_ID
static XIntc sIntc;
#else
static XScuGic sIntc;
#endif

//
// Interrupt vector table
#ifdef XPAR_INTC_0_DEVICE_ID
const ivt_t ivt[] = {
    // IIC
    {XPAR_AXI_INTC_0_AXI_IIC_0_IIC2INTC_IRPT_INTR, (XInterruptHandler)XIic_InterruptHandler, &sIic},
    // DMA Stream to MemoryMap Interrupt handler
    {XPAR_AXI_INTC_0_AXI_DMA_0_S2MM_INTROUT_INTR, (XInterruptHandler)fnS2MMInterruptHandler, &sAxiDma},
    // DMA MemoryMap to Stream Interrupt handler
    {XPAR_AXI_INTC_0_AXI_DMA_0_MM2S_INTROUT_INTR, (XInterruptHandler)fnMM2SInterruptHandler, &sAxiDma},
    // User I/O (buttons, switches, LEDs)
    {XPAR_AXI_INTC_0_AXI_GPIO_0_IP2INTC_IRPT_INTR, (XInterruptHandler)fnUserIOIsr, &sUserIO}};
#else
const ivt_t ivt[] = {
    // IIC
    {XPAR_FABRIC_AXI_IIC_0_IIC2INTC_IRPT_INTR, (Xil_ExceptionHandler)XIic_InterruptHandler, &sIic},
    // DMA Stream to MemoryMap Interrupt handler
    {XPAR_FABRIC_AXI_DMA_0_S2MM_INTROUT_INTR, (Xil_ExceptionHandler)fnS2MMInterruptHandler, &sAxiDma},
    // DMA MemoryMap to Stream Interrupt handler
    {XPAR_FABRIC_AXI_DMA_0_MM2S_INTROUT_INTR, (Xil_ExceptionHandler)fnMM2SInterruptHandler, &sAxiDma},
    // User I/O (buttons, switches, LEDs)
    {XPAR_FABRIC_AXI_GPIO_0_IP2INTC_IRPT_INTR, (Xil_ExceptionHandler)fnUserIOIsr, &sUserIO}};
#endif

//------- SD param--------

#define MAX_LOG_NUM 5

FIL *fptr;

static void SysMonInterruptHandler(void *CallBackRef);
static int SysMonSetupInterruptSystem(XScuGic *IntcInstancePtr,
                                      XSysMon *SysMonPtr,
                                      u16 IntrId);
int SysMonFractionToInt(float FloatNum);
int logNum = 0;

char dataBuffer[1024];
char *dataPntr = dataBuffer;

XSysMon SysMonInst;          /* System Monitor driver instance */
XScuGic InterruptController; /* Instance of the XIntc driver. */

#define MY_PWM 0x40004000 // This value is found in the Address editor tab in Vivado (next to Diagram tab)

//------ GPIO Parameter definitions --------

// Define los leds que no necesitan interrupcion
#define INTC_DEVICE_ID XPAR_PS7_SCUGIC_0_DEVICE_ID
#define LEDS_DEVICE_ID XPAR_AXI_GPIO_LED_DEVICE_ID
// los botones y su controlador
#define BTNS_DEVICE_ID XPAR_AXI_GPIO_BUTTONS_DEVICE_ID
#define INTC_GPIO_INTERRUPT_ID XPAR_FABRIC_AXI_GPIO_BUTTONS_IP2INTC_IRPT_INTR
// los switches y su controlador
#define SWS_DEVICE_ID XPAR_AXI_GPIO_SWITCHES_DEVICE_ID
#define INTCS_GPIO_INTERRUPT_ID XPAR_FABRIC_AXI_GPIO_SWITCHES_IP2INTC_IRPT_INTR // S de sw

// el timer y su controlador
#define TMR_DEVICE_ID XPAR_TMRCTR_0_DEVICE_ID
#define INTC_TMR_INTERRUPT_ID XPAR_FABRIC_AXI_TIMER_0_INTERRUPT_INTR
#define INTC_DEVICE_ID XPAR_PS7_SCUGIC_0_DEVICE_ID

// contador asociado al AXI timer es de 32 bits (0xFFFFFFFF)
// el Timer esta siendo alimentado con un reloj de 50 Mhz (50000000)
// empieza a contar desde TMR_LOAD hasta 0xFFFFFFFF con una frecuencia de 50MHz
// para obtener un tiempo de T segundos
#define TMR_LOAD (0xFFFFFFFF - 100000000 * 0.1) // 0.1s Timer
#define SW_INT XGPIO_IR_CH1_MASK
#define BTN_INT XGPIO_IR_CH1_MASK

// Variables para leds, botones, switches, interrupciones y timer
XGpio LEDInst;
XGpio BTNInst;
XGpio SWInst;
XScuGic INTCInst; // se usa para btn y sw
XTmrCtr TMRInst;

static int led_value;
static int btn_value;
static int sw_value;
static int tmr_count;
static int btn_press = 0;

int aux = 0;

//----------- Handlers ----------

static void TMR_Intr_Handler(void *baseaddr_p); //(en ayudantias)
static void BTN_Intr_Handler(void *baseaddr_p); //(en ayudantias)
static void SW_Intr_Handler(void *baseaddr_p);

//----------- Controladores de las interrupciones ----------
// relacionado al timer (TMR)
static int InterruptSystemSetup(XScuGic *XScuGicInstancePtr);       //(en ayudantias)
static int IntcInitFunction(u16 DeviceId, XTmrCtr *TmrInstancePtr); //(en ayudantias)
// relacionado a la instancia del gpio (BTN)
static int InterruptSystemSetup2(XScuGic *XScuGicInstancePtr);      //(en ayudantias)
static int IntcInitFunction2(u16 DeviceId, XGpio *GpioInstancePtr); //(en ayudantias)
// relacionado a la instancia del gpio (SW)
static int InterruptSystemSetup3(XScuGic *XScuGicInstancePtr);
static int IntcInitFunction3(u16 DeviceId, XGpio *GpioInstancePtr);

//----------- Funciones de SD ----------
static void SysMonInterruptHandler(void *CallBackRef);
static int SysMonSetupInterruptSystem(XScuGic *IntcInstancePtr,
                                      XSysMon *SysMonPtr,
                                      u16 IntrId);

//----------- Instanciaciones funciones propias ----------

void delay_ds(int delay);
void print_menu(void);
void flashear(void);

//------Structs--------

struct canciones
{
    char nombre[50];
    char target[50];
    short unsigned int id;
    short unsigned int usado;
};

//-----------Inicio MAIN ----------
int main()
{
	init_platform();
    int Status;

    Demo.u8Verbose = 0;

    // Xil_DCacheDisable();

    xil_printf("\r\n--- Entering main() --- \r\n");

    //
    // Initialize the interrupt controller

    Status = fnInitInterruptController(&sIntc);
    if (Status != XST_SUCCESS)
    {
        xil_printf("Error initializing interrupts");
        return XST_FAILURE;
    }

    // Initialize IIC controller
    Status = fnInitIic(&sIic);
    if (Status != XST_SUCCESS)
    {
        xil_printf("Error initializing I2C controller");
        return XST_FAILURE;
    }

    // Initialize User I/O driver
    Status = fnInitUserIO(&sUserIO);
    if (Status != XST_SUCCESS)
    {
        xil_printf("User I/O ERROR");
        return XST_FAILURE;
    }

    // Initialize DMA
    Status = fnConfigDma(&sAxiDma);
    if (Status != XST_SUCCESS)
    {
        xil_printf("DMA configuration ERROR");
        return XST_FAILURE;
    }

    // Initialize Audio I2S
    Status = fnInitAudio();
    if (Status != XST_SUCCESS)
    {
        xil_printf("Audio initializing ERROR");
        return XST_FAILURE;
    }

    {
        XTime tStart, tEnd;

        XTime_GetTime(&tStart);
        do
        {
            XTime_GetTime(&tEnd);
        } while ((tEnd - tStart) / (COUNTS_PER_SECOND / 10) < 20);
    }
    // Initialize Audio I2S
    Status = fnInitAudio();
    if (Status != XST_SUCCESS)
    {
        xil_printf("Audio initializing ERROR");
        return XST_FAILURE;
    }

    // Enable all interrupts in our interrupt vector table
    // Make sure all driver instances using interrupts are initialized first
    fnEnableInterrupts(&sIntc, &ivt[0], sizeof(ivt) / sizeof(ivt[0]));
    int status;
    //----------------------------------------------------
    // INITIALIZE THE PERIPHERALS & SET DIRECTIONS OF GPIO
    //----------------------------------------------------
    // Initialize LEDs
    status = XGpio_Initialize(&LEDInst, LEDS_DEVICE_ID);
    if (status != XST_SUCCESS)
        return XST_FAILURE;
    // Initialize Push Buttons
    status = XGpio_Initialize(&BTNInst, BTNS_DEVICE_ID);
    if (status != XST_SUCCESS)
        return XST_FAILURE;
    // Initialize Switches
    status = XGpio_Initialize(&SWInst, SWS_DEVICE_ID);
    if (status != XST_SUCCESS)
        return XST_FAILURE;
    

    // Set LEDs direction to outputs
    // setea los leds como outputs (ceros), si fueran 1 serian entradas
    XGpio_SetDataDirection(&LEDInst, 1, 0x00);

    // Set all buttons direction to inputs
    // 0xFF indica que son entradas
    XGpio_SetDataDirection(&BTNInst, 1, 0xFF);

    // Set all switches direction to inputs
    XGpio_SetDataDirection(&SWInst, 1, 0xFF);
    

    //----------------------------------------------------
    // SETUP THE TIMER
    //----------------------------------------------------
    status = XTmrCtr_Initialize(&TMRInst, TMR_DEVICE_ID);
    if (status != XST_SUCCESS)
        return XST_FAILURE;
    XTmrCtr_SetHandler(&TMRInst, (XTmrCtr_Handler)TMR_Intr_Handler, &TMRInst);
    // el valor de reset es del cual empieza a contar hasta los 32 bits a una frecuencia de 50MHz
    XTmrCtr_SetResetValue(&TMRInst, 0, TMR_LOAD);                                  // setea el valor del reset llamado TMR_LOAD
    XTmrCtr_SetOptions(&TMRInst, 0, XTC_INT_MODE_OPTION | XTC_AUTO_RELOAD_OPTION); // setea opciones
    
    // Initialize interrupt controller (controlador de interrupciones)
    status = IntcInitFunction(INTC_DEVICE_ID, &TMRInst);
    if (status != XST_SUCCESS)
        return XST_FAILURE;
    XTmrCtr_Start(&TMRInst, 0);
    

    // Initialize interrupt controller del boton
    status = IntcInitFunction2(INTC_DEVICE_ID, &BTNInst);
    if (status != XST_SUCCESS)
        return XST_FAILURE;
    // Initialize interrupt controller de los sw
    status = IntcInitFunction3(INTC_DEVICE_ID, &SWInst);
    if (status != XST_SUCCESS)
        return XST_FAILURE;
    
    // Creamos estructura de canciones

    struct canciones cancion[4], *ptr;
    ptr = &cancion[0];
    int flag = 0;
    int flag2 = 0;
    int num = 0;
    char name[50];
    int go =0;
    
    

    while(1){
    switch (flag)
    {
    case 0: // Menu
        flashear();
        print_menu();
        go =0;

        flag = sw_value;

        break;

    case 1: // Ingresar nombre e ID de la canci??n

        xil_printf("Para elegir la canci??n seleccione con el switch y presione un boton para confirmar\r");
       
        if (btn_value>0)
        {
            ptr = &cancion[sw_value];
            go = 1;
        }

        if (go > 0)
        {
            xil_printf("Ingrese nombre de la cancion: ");
            scanf("%s", name);
            strncpy((ptr->nombre), name, 50);
            (ptr->id) = sw_value;
            (ptr->usado) = 1;
            strcat(name, ".txt");
            strncpy((ptr->target), name, 50);
            xil_printf("name is %s, saved in %s \n", (ptr->nombre), (ptr->target));
            flag =0;
        }

            break;

        case 2: // Grabar la canci??n
            switch (flag2)
            {
            case 0:

                Xil_Out32(MY_PWM, 4*num);
                Xil_Out32(MY_PWM + 4, 0);
                Xil_Out32(MY_PWM + 8, 0);
                num++;
                delay_ds(1);
                if (num>254){
                    flag2++;

                }
                break;
            case 1:
                Xil_Out32(MY_PWM, 4 * num);
                Xil_Out32(MY_PWM + 4, 4*num);
                Xil_Out32(MY_PWM + 8, 0);
                num--;
                delay_ds(1);
                if (num <= 0)
                {
                    flag2++;
                }
                break;
            case 2:
                
                Xil_Out32(MY_PWM, 0);
                Xil_Out32(MY_PWM + 4, 4 * num);
                Xil_Out32(MY_PWM + 8, 0);
                num++;
                delay_ds(1);
                if (num > 254)
                {
                    flag2++;
                }
                break;
            case 3:
                Xil_Out32(MY_PWM, 0);
                Xil_Out32(MY_PWM + 4, 4 * num);
                Xil_Out32(MY_PWM + 8, 4 * num);
                num--;
                delay_ds(1);
                if (num <= 0)
                {
                    flag2++;
                }
                break;
            case 4:
                Xil_Out32(MY_PWM, 0);
                Xil_Out32(MY_PWM + 4, 0);
                Xil_Out32(MY_PWM + 8, 4 * num);
                num++;
                delay_ds(1);
                if (num > 254)
                {
                    flag2++;
                }
                break;
            case 5:
                Xil_Out32(MY_PWM, 4 * num);
                Xil_Out32(MY_PWM + 4, 0);
                Xil_Out32(MY_PWM + 8, 4 * num);
                num--;
                delay_ds(1);
                if (num <= 0)
                {
                    flag2++;
                }
                break;
            case 6:
                Xil_Out32(MY_PWM, 4 * num);
                Xil_Out32(MY_PWM + 4, 4 * num);
                Xil_Out32(MY_PWM + 8, 4 * num);
                num++;
                delay_ds(1);
                if (num > 254)
                {
                    flag2=0;
                    flag=0;
                }
                break;

            
            default:
                Xil_Out32(MY_PWM, 0);
                Xil_Out32(MY_PWM + 4, 0);
                Xil_Out32(MY_PWM + 8, 0);
                break;
            }
            break;

        case 3: // Reproducir canci??n
            if (Demo.fDmaS2MMEvent)
            {
                xil_printf("\r\nRecording Done...");

                // Disable Stream function to send data (S2MM)
                Xil_Out32(I2S_STREAM_CONTROL_REG, 0x00000000);
                Xil_Out32(I2S_TRANSFER_CONTROL_REG, 0x00000000);

                Xil_DCacheInvalidateRange((u32)MEM_BASE_ADDR, 5 * NR_AUDIO_SAMPLES);
                // microblaze_invalidate_dcache();
                //  Reset S2MM event and record flag
                Demo.fDmaS2MMEvent = 0;
                Demo.fAudioRecord = 0;
            }

            // Checking the DMA MM2S event flag
            if (Demo.fDmaMM2SEvent)
            {
                xil_printf("\r\nPlayback Done...");

                // Disable Stream function to send data (S2MM)
                Xil_Out32(I2S_STREAM_CONTROL_REG, 0x00000000);
                Xil_Out32(I2S_TRANSFER_CONTROL_REG, 0x00000000);
                // Flush cache
                //					//microblaze_flush_dcache();
                Xil_DCacheFlushRange((u32)MEM_BASE_ADDR, 5 * NR_AUDIO_SAMPLES);
                // Reset MM2S event and playback flag
                Demo.fDmaMM2SEvent = 0;
                Demo.fAudioPlayback = 0;
            }

            // Checking the DMA Error event flag
            if (Demo.fDmaError)
            {
                xil_printf("\r\nDma Error...");
                xil_printf("\r\nDma Reset...");

                Demo.fDmaError = 0;
                Demo.fAudioPlayback = 0;
                Demo.fAudioRecord = 0;
            }

            // Checking the btn change event
            if (Demo.fUserIOEvent)
            {

                switch (Demo.chBtn)
                {
                case 'u':
                    if (!Demo.fAudioRecord && !Demo.fAudioPlayback)
                    {
                        xil_printf("\r\nStart Recording...\r\n");
                        fnSetMicInput();

                        fnAudioRecord(sAxiDma, NR_AUDIO_SAMPLES);
                        Demo.fAudioRecord = 1;
                    }
                    else
                    {
                        if (Demo.fAudioRecord)
                        {
                            xil_printf("\r\nStill Recording...\r\n");
                        }
                        else
                        {
                            xil_printf("\r\nStill Playing back...\r\n");
                        }
                    }
                    break;
                case 'd':
                    if (!Demo.fAudioRecord && !Demo.fAudioPlayback)
                    {
                        xil_printf("\r\nStart Playback...\r\n");
                        fnSetHpOutput();
                        fnAudioPlay(sAxiDma, NR_AUDIO_SAMPLES);
                        Demo.fAudioPlayback = 1;
                    }
                    else
                    {
                        if (Demo.fAudioRecord)
                        {
                            xil_printf("\r\nStill Recording...\r\n");
                        }
                        else
                        {
                            xil_printf("\r\nStill Playing back...\r\n");
                        }
                    }
                    break;
                case 'r':
                    if (!Demo.fAudioRecord && !Demo.fAudioPlayback)
                    {
                        xil_printf("\r\nStart Recording...\r\n");
                        fnSetLineInput();
                        fnAudioRecord(sAxiDma, NR_AUDIO_SAMPLES);
                        Demo.fAudioRecord = 1;
                    }
                    else
                    {
                        if (Demo.fAudioRecord)
                        {
                            xil_printf("\r\nStill Recording...\r\n");
                        }
                        else
                        {
                            xil_printf("\r\nStill Playing back...\r\n");
                        }
                    }
                    break;
                case 'l':
                    if (!Demo.fAudioRecord && !Demo.fAudioPlayback)
                    {
                        xil_printf("\r\nStart Playback...");
                        fnSetLineOutput();
                        fnAudioPlay(sAxiDma, NR_AUDIO_SAMPLES);
                        Demo.fAudioPlayback = 1;
                    }
                    else
                    {
                        if (Demo.fAudioRecord)
                        {
                            xil_printf("\r\nStill Recording...\r\n");
                        }
                        else
                        {
                            xil_printf("\r\nStill Playing back...\r\n");
                        }
                    }
                    break;
                default:
                    break;
                }

                // Reset the user I/O flag
                Demo.chBtn = 0;
                Demo.fUserIOEvent = 0;
            }
            break;
            
        case 4:
            xil_printf("Creando archivo")
            break;

        default:
            flag = 0;
            break;
        }
    }
    cleanup_platform();
}

//-----------Fin MAIN ----------

void delay_ds(int delay)
{
    int contador_t;
    contador_t = tmr_count;
    while (tmr_count < contador_t + delay)
    {
        xil_printf(" ");
    }
    xil_printf("\r");
    // tmr_count = 0;
}
void print_menu(void)
{
    xil_printf("--------------------------\r Menu: Utilice los sw para seleccinar una opci??n\r 1) Ingresar una nueva canci??n\r 2) RGB demo\r 3) Audio demo\r 4) SD demo\r --------------------------\r");
}
void flashear(void)
{
	xil_printf("flasheando \r");
    XGpio_DiscreteWrite(&LEDInst, 1, 15);
    delay_ds(5);
    XGpio_DiscreteWrite(&LEDInst, 1, 0);
    delay_ds(5);
    XGpio_DiscreteWrite(&LEDInst, 1, 15);
    delay_ds(5);
    XGpio_DiscreteWrite(&LEDInst, 1, 0);
    delay_ds(5);
}

//----------- DEFINICION DE FUNCIONES PARA SISTEMA
// Manipulador de la interrupcion del timer
void TMR_Intr_Handler(void *data)
{
    if (XTmrCtr_IsExpired(&TMRInst, 0))
    {
        // si controlador del timer expira, debe resetear
        XTmrCtr_Reset(&TMRInst, 0);

        tmr_count++;
    }
}
void BTN_Intr_Handler(void *InstancePtr)
{
    // Disable GPIO interrupts
    XGpio_InterruptDisable(&BTNInst, BTN_INT);
    // Ignore additional button presses
    // Solo reaccionar cuando se presiona el boton y no cuando se suelta (debouncer)
    if ((XGpio_InterruptGetStatus(&BTNInst) & BTN_INT) != BTN_INT)
    {
        return;
    }
    btn_value = XGpio_DiscreteRead(&BTNInst, 1);
    //se activa solo cuando se presiona, no cuando se suelta el boton
    btn_press = 0;
    if(btn_value != 0){
       	btn_press = 1;
    };
    XGpio_DiscreteWrite(&LEDInst, 1, btn_value);

    xil_printf("ha cambiado el btn \r");
    xil_printf("val btn %d \r",btn_value);
    (void)XGpio_InterruptClear(&BTNInst, BTN_INT);
    // Enable GPIO interrupts
    XGpio_InterruptEnable(&BTNInst, BTN_INT);

}
void SW_Intr_Handler(void *InstancePtr)
{
    // Disable GPIO interrupts
    XGpio_InterruptDisable(&SWInst, SW_INT);
    // Ignore additional switches moves

    // Solo reaccionar cuando se acciona el switch y no cuando se suelta (debouncer)
    if ((XGpio_InterruptGetStatus(&SWInst) & SW_INT) != SW_INT)
    {
        return;
    }

    sw_value = XGpio_DiscreteRead(&SWInst, 1);
    xil_printf("ha cambiado el switch\r");
    xil_printf("val switch %d \r", sw_value);

    (void)XGpio_InterruptClear(&SWInst, SW_INT);
    // Enable GPIO interrupts
    XGpio_InterruptEnable(&SWInst, SW_INT);
}

//-----------  Interrupts Setup -----------

int InterruptSystemSetup(XScuGic *XScuGicInstancePtr)
{
	xil_printf("100Iniciando antes...\r"); //Si imprime
    Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT,
                                 (Xil_ExceptionHandler)XScuGic_InterruptHandler,
                                 XScuGicInstancePtr);
    Xil_ExceptionEnable();
    return XST_SUCCESS;
}
// Para botones
int InterruptSystemSetup2(XScuGic *XScuGicInstancePtr)
{
    // Enable interrupt
    XGpio_InterruptEnable(&BTNInst, BTN_INT);
    XGpio_InterruptGlobalEnable(&BTNInst);
    Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT,
                                 (Xil_ExceptionHandler)XScuGic_InterruptHandler,
                                 XScuGicInstancePtr);
    Xil_ExceptionEnable();
    return XST_SUCCESS;
}

// Para switches
int InterruptSystemSetup3(XScuGic *XScuGicInstancePtr)
{
    // Enable interrupt
    XGpio_InterruptEnable(&SWInst, SW_INT);
    XGpio_InterruptGlobalEnable(&SWInst);
    Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT,
                                 (Xil_ExceptionHandler)XScuGic_InterruptHandler,
                                 XScuGicInstancePtr);
    Xil_ExceptionEnable();
    return XST_SUCCESS;
}

//-----------  Init Functions -----------

// Para timer
int IntcInitFunction(u16 DeviceId, XTmrCtr *TmrInstancePtr)
{
    XScuGic_Config *IntcConfig;
    int status;
    xil_printf("90Iniciando antes...\r"); //Si imprime
    // Interrupt controller initialization
    // le asigna la configuracion que encontro  del device (lookup) del device
    IntcConfig = XScuGic_LookupConfig(DeviceId);
    xil_printf("91Iniciando antes...\r"); //Si imprime
    status = XScuGic_CfgInitialize(&INTCInst, IntcConfig, IntcConfig->CpuBaseAddress); // lo inicializa
    if (status != XST_SUCCESS)
        return XST_FAILURE;
    xil_printf("92Iniciando antes...\r"); //Si imprime
    // Call to interrupt setup, llama a la funcion InterruptSystemSetup que definimos en este codigo
    status = InterruptSystemSetup(&INTCInst);
    if (status != XST_SUCCESS)
        return XST_FAILURE;
    xil_printf("93Iniciando antes...\r"); //Si imprime
    // Connect timer interrupt to handler
    status = XScuGic_Connect(&INTCInst, INTC_TMR_INTERRUPT_ID,
    		(Xil_ExceptionHandler)TMR_Intr_Handler,
			(void *)TmrInstancePtr);
    if (status != XST_SUCCESS)
        return XST_FAILURE;
    //Enable timer interrupts in the controller
    XScuGic_Enable(&INTCInst, INTC_TMR_INTERRUPT_ID);
    return XST_SUCCESS;
}

// Para botones
int IntcInitFunction2(u16 DeviceId, XGpio *GpioInstancePtr)
{
    XScuGic_Config *IntcConfig;
    int status;
    // Interrupt controller initialization
    IntcConfig = XScuGic_LookupConfig(DeviceId);
    status = XScuGic_CfgInitialize(&INTCInst, IntcConfig, IntcConfig->CpuBaseAddress);
    if (status != XST_SUCCESS)
        return XST_FAILURE;

    // Call to interrupt setup
    status = InterruptSystemSetup2(&INTCInst);
    if (status != XST_SUCCESS)
        return XST_FAILURE;

    // Connect GPIO interrupt to handler
    status = XScuGic_Connect(&INTCInst,
                             INTC_GPIO_INTERRUPT_ID,
                             (Xil_ExceptionHandler)BTN_Intr_Handler,
                             (void *)GpioInstancePtr);
    if (status != XST_SUCCESS)
        return XST_FAILURE;

    // Enable GPIO interrupts interrupt
    XGpio_InterruptEnable(GpioInstancePtr, 1);
    XGpio_InterruptGlobalEnable(GpioInstancePtr);

    // Enable GPIO and timer interrupts in the controller
    XScuGic_Enable(&INTCInst, INTC_GPIO_INTERRUPT_ID);

    return XST_SUCCESS;
}

// Para switches
int IntcInitFunction3(u16 DeviceId, XGpio *GpioInstancePtr)
{
    XScuGic_Config *IntcConfig;
    int status;
    // Interrupt controller initialization
    IntcConfig = XScuGic_LookupConfig(DeviceId);
    status = XScuGic_CfgInitialize(&INTCInst, IntcConfig, IntcConfig->CpuBaseAddress);
    if (status != XST_SUCCESS)
        return XST_FAILURE;

    // Call to interrupt setup
    status = InterruptSystemSetup3(&INTCInst);
    if (status != XST_SUCCESS)
        return XST_FAILURE;

    // Connect GPIO interrupt to handler
    status = XScuGic_Connect(&INTCInst,
                             INTCS_GPIO_INTERRUPT_ID,
                             (Xil_ExceptionHandler)SW_Intr_Handler,
                             (void *)GpioInstancePtr);
    if (status != XST_SUCCESS)
        return XST_FAILURE;

    // Enable GPIO interrupts interrupt
    XGpio_InterruptEnable(GpioInstancePtr, 1);
    XGpio_InterruptGlobalEnable(GpioInstancePtr);

    // Enable GPIO and timer interrupts in the controller
    XScuGic_Enable(&INTCInst, INTCS_GPIO_INTERRUPT_ID);

    return XST_SUCCESS;
}

//-----------  SD Functions -----------
static void SysMonInterruptHandler(void *CallBackRef)
{
    u32 IntrStatusValue;
    u16 TempRawData;
    float TempData;
    XSysMon *SysMonPtr = (XSysMon *)CallBackRef;
    /*
     * Get the interrupt status from the device and check the value.
     */
    XSysMon_IntrGlobalDisable(&SysMonInst);
    IntrStatusValue = XSysMon_IntrGetStatus(SysMonPtr);
    XSysMon_IntrClear(SysMonPtr, IntrStatusValue);
    logNum++;

    if (IntrStatusValue & XSM_IPIXR_EOC_MASK)
    {
        TempRawData = XSysMon_GetAdcData(&SysMonInst, XSM_CH_TEMP);
        TempData = XSysMon_RawToTemperature(TempRawData);
        printf("\r\nThe Current Temperature is %0.3f Centigrades. %d\r\n", TempData, logNum);
        sprintf(dataPntr, "%0.3f\n", TempData);
        dataPntr = dataPntr + 8;

        if (logNum % 10 == 0)
        {
            xil_printf("Updating SD card...\n\r");
            writeFile(fptr, 80, (u32)dataBuffer);
            dataPntr = (char *)dataBuffer;
        }

        if (logNum == MAX_LOG_NUM)
        {
            closeFile(fptr);
            SD_Eject();
            xil_printf("Safe to remove SD Card...\n\r");
            XSysMon_IntrGlobalDisable(&SysMonInst);
        }
        else
        {
            sleep(1);
            XSysMon_IntrGlobalEnable(&SysMonInst);
        }
    }
}

static int SysMonSetupInterruptSystem(XScuGic *IntcInstancePtr,
                                      XSysMon *SysMonPtr,
                                      u16 IntrId)
{
    int Status;
    XScuGic_Config *IntcConfig;
    IntcConfig = XScuGic_LookupConfig(XPAR_SCUGIC_SINGLE_DEVICE_ID);
    if (NULL == IntcConfig)
    {
        return XST_FAILURE;
    }

    Status = XScuGic_CfgInitialize(IntcInstancePtr, IntcConfig,
                                   IntcConfig->CpuBaseAddress);
    if (Status != XST_SUCCESS)
    {
        return XST_FAILURE;
    }
    XScuGic_SetPriorityTriggerType(IntcInstancePtr, IntrId,
                                   0xA0, 0x3);
    Status = XScuGic_Connect(IntcInstancePtr, IntrId,
                             (Xil_ExceptionHandler)SysMonInterruptHandler,
                             SysMonPtr);
    if (Status != XST_SUCCESS)
    {
        return Status;
    }
    XScuGic_Enable(IntcInstancePtr, IntrId);

    Xil_ExceptionInit();
    Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT, (Xil_ExceptionHandler)XScuGic_InterruptHandler, (void *)IntcInstancePtr);
    Xil_ExceptionEnable();

    return XST_SUCCESS;
}
