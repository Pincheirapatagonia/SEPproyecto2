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

//------- SD param--------

#define MAX_LOG_NUM 20

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

// #define MY_PWM XPAR_MY_PWM_CORE_0_S00_AXI_BASEADDR //Because of a bug in Vivado 2015.3 and 2015.4, this value is not correct.
#define MY_PWM 0x40004000 // This value is found in the Address editor tab in Vivado (next to Diagram tab)

// los switches y su controlador
#define SWS_DEVICE_ID XPAR_AXI_GPIO_SWITCHES_DEVICE_ID
#define INTCS_GPIO_INTERRUPT_ID XPAR_FABRIC_AXI_GPIO_SWITCHES_IP2INTC_IRPT_INTR // S de sw

// el timer y su controlador
#define TMR_DEVICE_ID XPAR_TMRCTR_0_DEVICE_ID
#define INTC_TMR_INTERRUPT_ID XPAR_FABRIC_AXI_TIMER_0_INTERRUPT_INTR
#define INTC_DEVICE_ID XPAR_PS7_SCUGIC_0_DEVICE_ID
// contador asociado al AXI timer es 32 bits (0xFFFFFFFF)
// el Timer est  siendo alimentado con un reloj de 50 Mhz (50000000)
// empieza a contar desde TMR_LOAD hasta 0xFFFFFFFF con una frecuencia de 50MHz
// para obtener un tiempo de T segundos
#define TMR_LOAD (0xFFFFFFFF - 100000000 * 0.1) // 0.1s Timer
#define SW_INT XGPIO_IR_CH1_MASK

XGpio BTNInst;
XGpio SWInst;
XScuGic INTCInst; // se usa para btn y sw
XTmrCtr TMRInst;

static int sw_value;
static int tmr_count;

// static void TMR_Intr_Handler(void *baseaddr_p);
// handler (manipulador) de la interrupcion del timer
static void TMR_Intr_Handler(void *baseaddr_p); //(en ayudantias)

static void SW_Intr_Handler(void *baseaddr_p);

//----------- Controladores de las interrupciones ----------
// relacionado al timer (TMR)

static int IntcInitFunction(u16 DeviceId, XTmrCtr *TmrInstancePtr); //(en ayudantias)

// relacionado a la instancia del gpio (SW)
static int InterruptSystemSetup3(XScuGic *XScuGicInstancePtr);
static int IntcInitFunction3(u16 DeviceId, XGpio *GpioInstancePtr);

void delay_ds(int delay);

int aux = 0;
int main()
{

    int flag;


    switch (flag)
    {
    case 0: //Menu
        
        if(btn_value != 0){}
        break;
    
    case 1: //Ingresar nombre e ID de la canción




    case 2: // Grabar la canción

    case 3: // Reproducir canción

    default:
        break;
    }








    
}

// Fin del main
void delay_ds(int delay)
{
    int contador_t;
    contador_t = tmr_count;
    while (tmr_count < contador_t + delay)
    {
        xil_printf(" ");
    }
    // tmr_count = 0;
}

void TMR_Intr_Handler(void *data)
{
    if (XTmrCtr_IsExpired(&TMRInst, 0))
    {
        // si controlador del timer expira, debe resetear
        XTmrCtr_Reset(&TMRInst, 0);

        tmr_count++;
    }
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
    xil_printf("ha cambiado la pos del switch\r");
    xil_printf("val switch %d", sw_value);
    // XGpio_DiscreteWrite(&LEDInst, 1, sw_value);

    (void)XGpio_InterruptClear(&SWInst, SW_INT);
    // Enable GPIO interrupts
    XGpio_InterruptEnable(&SWInst, SW_INT);
}

// Funcion relacionada al timer
int IntcInitFunction(u16 DeviceId, XTmrCtr *TmrInstancePtr)
{
    XScuGic_Config *IntcConfig;
    int status;

    // Interrupt controller initialization
    // le asigna la configuracion que encontr  del device (lookup) del device
    IntcConfig = XScuGic_LookupConfig(DeviceId);
    status = XScuGic_CfgInitialize(&INTCInst, IntcConfig, IntcConfig->CpuBaseAddress); // lo inicializa
    if (status != XST_SUCCESS)
        return XST_FAILURE;

    // Call to interrupt setup
    status = InterruptSystemSetup(&INTCInst); // llama a la funcion InterruptSystemSetup que definimos en este c digo
    if (status != XST_SUCCESS)
        return XST_FAILURE;
    // Funcion relacionada al sistema de gpio (botones, switches)
}

// probando los switches
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
