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

#include "xdebug.h"

#include <string.h>

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

#define MY_PWM 0x43C10000 // This value is found in the Address editor tab in Vivado (next to Diagram tab)

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
    int go = 0;

    while (1)
    {
        switch (flag)
        {
        case 0: // Menu

            print_menu();
            go = 0;
            flag = 5;

            break;
        case 5:
            flashear();
            if (sw_value!=0){
                flag = sw_value;
            }
            
            break;
        case 1: // Ingresar nombre e ID de la canción

            xil_printf("Para elegir la canción seleccione con el switch y presione un boton para confirmar\r");

            flag = 6;

            break;
        case 6:
            xil_printf
            if (btn_value > 0)
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
                flag = 0;
            }
            break;
        case 2: // RGB
            xil_printf("Entrando a caso 2\r");
            switch (flag2)
            {
            case 0:

                Xil_Out32(MY_PWM, 4 * num);
                Xil_Out32(MY_PWM + 4, 0);
                Xil_Out32(MY_PWM + 8, 0);
                num++;
                delay_ds(1);
                if (num > 254)
                {
                    flag2++;
                }
                break;
            case 1:
                Xil_Out32(MY_PWM, 4 * num);
                Xil_Out32(MY_PWM + 4, 4 * num);
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
                    flag2 = 0;
                    flag = 0;
                }
                break;

            default:
                Xil_Out32(MY_PWM, 0);
                Xil_Out32(MY_PWM + 4, 0);
                Xil_Out32(MY_PWM + 8, 0);
                break;
            }
            break;

        case 3: // Reproducir canción
            xil_printf("Caso 3");
            break;

        case 4:
            xil_printf("Creando archivo");
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
    int aux;
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
    xil_printf("--------------------------\r Menu: Utilice los sw para seleccionar una opción\r 1) Ingresar una nueva canción\r 2) RGB demo\r 3) Audio demo\r 4) SD demo\r --------------------------\r");
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
    // se activa solo cuando se presiona, no cuando se suelta el boton
    btn_press = 0;
    if (btn_value != 0)
    {
        btn_press = 1;
    };
    XGpio_DiscreteWrite(&LEDInst, 1, btn_value);

    xil_printf("\r ha cambiado el btn \r");
    xil_printf("val btn %d \r", btn_value);
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
    xil_printf("\r ha cambiado el switch\r");
    xil_printf("val switch %d \r", sw_value);

    (void)XGpio_InterruptClear(&SWInst, SW_INT);
    // Enable GPIO interrupts
    XGpio_InterruptEnable(&SWInst, SW_INT);
}

//-----------  Interrupts Setup -----------

int InterruptSystemSetup(XScuGic *XScuGicInstancePtr)
{
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
    // Interrupt controller initialization
    // le asigna la configuracion que encontro  del device (lookup) del device
    IntcConfig = XScuGic_LookupConfig(DeviceId);
    status = XScuGic_CfgInitialize(&INTCInst, IntcConfig, IntcConfig->CpuBaseAddress); // lo inicializa
    if (status != XST_SUCCESS)
        return XST_FAILURE;
    // Call to interrupt setup, llama a la funcion InterruptSystemSetup que definimos en este codigo
    status = InterruptSystemSetup(&INTCInst);
    if (status != XST_SUCCESS)
        return XST_FAILURE;
    // Connect timer interrupt to handler
    status = XScuGic_Connect(&INTCInst, INTC_TMR_INTERRUPT_ID,
                             (Xil_ExceptionHandler)TMR_Intr_Handler,
                             (void *)TmrInstancePtr);
    if (status != XST_SUCCESS)
        return XST_FAILURE;
    // Enable timer interrupts in the controller
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