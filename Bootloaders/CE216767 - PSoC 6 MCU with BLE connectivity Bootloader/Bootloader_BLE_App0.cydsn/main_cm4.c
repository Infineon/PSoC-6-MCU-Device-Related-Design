/*******************************************************************************
* File Name: main_cm4.c
*
* Version: 1.20
*
* Description: This file provides the source code for the Bootloader (App0)
*              running on the Core M4 (Core1).
*              App0 Core1 firmware does the following:
*              - Bootloads App1 firmware image if Host sends it
*              - Switches to App1 if App1 image has successfully bootloaded
*                and is valid
*              - Switches to existing App1 if button is pressed
*              - Turn on an LED depending on status
*              - Hibernates on timeout
*******************************************************************************
* Related Document: CE216767.pdf
*
* Hardware Dependency: CY8CKIT-062-BLE PSoC 6 BLE Pioneer Kit
*                      CY5677 CySmart USB Dongle
*
******************************************************************************
* Copyright (2017), Cypress Semiconductor Corporation.
******************************************************************************
* This software is owned by Cypress Semiconductor Corporation (Cypress) and is
* protected by and subject to worldwide patent protection (United States and
* foreign), United States copyright laws and international treaty provisions.
* Cypress hereby grants to licensee a personal, non-exclusive, non-transferable
* license to copy, use, modify, create derivative works of, and compile the
* Cypress Source Code and derivative works for the sole purpose of creating
* custom software in support of licensee product to be used only in conjunction
* with a Cypress integrated circuit as specified in the applicable agreement.
* Any reproduction, modification, translation, compilation, or representation of
* this software except as specified above is prohibited without the express
* written permission of Cypress.
*
* Disclaimer: CYPRESS MAKES NO WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, WITH
* REGARD TO THIS MATERIAL, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
* Cypress reserves the right to make changes without further notice to the
* materials described herein. Cypress does not assume any liability arising out
* of the application or use of any product or circuit described herein. Cypress
* does not authorize its products for use as critical components in life-support
* systems where a malfunction or failure may reasonably be expected to result in
* significant injury to the user. The inclusion of Cypress' product in a life-
* support systems application implies that the manufacturer assumes all risk of
* such use and in doing so indemnifies Cypress against all charges. Use may be
* limited by and subject to the applicable Cypress software license agreement.
*******************************************************************************/

#include <string.h>
#include "project.h"
#include "debug.h"
#include "ias.h"
#include "transport_ble.h"

#if CY_BOOTLOAD_OPT_CRYPTO_HW != 0
    #include "cy_crypto_config.h"
    cy_stc_crypto_context_t cryptoContext;
#endif /* CY_BOOTLOAD_OPT_CRYPTO_HW != 0 */

/* BLE GAPP Connection Settings */
#define CYBLE_GAPP_CONNECTION_INTERVAL_MIN  (0x000Cu) /* 15 ms - (N * 1,25)*/
#define CYBLE_GAPP_CONNECTION_INTERVAL_MAX  (0x000Cu) /* 15 ms */
#define CYBLE_GAPP_CONNECTION_SLAVE_LATENCY (0x0000u)
#define CYBLE_GAPP_CONNECTION_TIME_OUT      (0x00C8u) /* 2000 ms */

/* BLE Callback function */
void AppCallBack(uint32 event, void* eventParam);

/* Internal functions */
static bool IsButtonPressed(uint16_t timeoutInMilis);
static uint32_t counterTimeoutSeconds(uint32_t seconds, uint32_t timeout);

static cy_en_bootload_status_t CopyRow(uint32_t dest, uint32_t src, uint32_t rowSize, cy_stc_bootload_params_t * params);
static cy_en_bootload_status_t HandleMetadata(cy_stc_bootload_params_t *params);

/*******************************************************************************
* Function Name: main
********************************************************************************
*
* Summary:
*  Main function of the bootloader application (App0).
*  1. If application started from Non-Software reset it validates App1
*  1.1. If App1 is valid it switches to App1, else goto #2.
*  2. Start bootloading communication.
*  3. If updated application has been received it validates this app.
*  4. If App1 is valid it switches to it, else wait for new application.
*  5. If 300 seconds have passed and no new application has been received
*     then validate App1, if it is valid then switch to it, else hibernate
*     (Happens in the BLE AppCallBack).
*
*******************************************************************************/
int main(void)
{
    /* timeout for Cy_Bootload_Continue(), in milliseconds */
    const uint32_t paramsTimeout = 20u;
    
    /* Bootloader params, used to configure bootloader */
    static cy_stc_bootload_params_t bootParams;
    
    /* Status codes for Bootloader SDK API */
    cy_en_bootload_status_t status;
    
    /* SW2 released after deciding to stay in bootloader */
    bool buttonReleased = false;
    
    /* 
    * Bootloading state, one of
    * - CY_BOOTLOAD_STATE_NONE
    * - CY_BOOTLOAD_STATE_BOOTLOADING
    * - CY_BOOTLOAD_STATE_FINISHED
    * - CY_BOOTLOAD_STATE_FAILED
    */
    uint32_t state = CY_BOOTLOAD_STATE_NONE;
    
    cy_en_ble_api_result_t apiResult;
    cy_stc_ble_stack_lib_version_t stackVersion;
    
    /*
    * Used to count seconds, to convert counts to seconds use
    * counterTimeoutSeconds(SECONDS, paramsTimeout)
    */
    uint32_t count = 0;
    
    uint32_t ledTimer = 0;
    
#if CY_BOOTLOAD_OPT_CRYPTO_HW != 0
    cy_en_crypto_status_t cryptoStatus;
#endif

    /* Buffer to store bootloader commands */
    CY_ALIGN(4) static uint8_t buffer[CY_BOOTLOAD_SIZEOF_DATA_BUFFER];

    /* Buffer for bootloader packets for Transport API */
    CY_ALIGN(4) static uint8_t packet[CY_BOOTLOAD_SIZEOF_CMD_BUFFER];    
    
    /* Enable global interrupts */
    __enable_irq();
    
    /* Start UART Services */
    UART_START();
    
    /* Initializes LEDs */
    InitLED();
    
#if CY_BOOTLOAD_OPT_CRYPTO_HW != 0
    /* Initialize the Crypto Client code */
    cryptoStatus = Cy_Crypto_Init(&cryptoConfig, &cryptoContext);
    if (cryptoStatus != CY_CRYPTO_SUCCESS)
    {
        /* Crypto not initialized, debug what is the problem */
        Cy_SysLib_Halt(0x00u);
    }
#endif /* CY_BOOTLOAD_OPT_CRYPTO_HW != 0 */

    /* Initialize bootParams structure and Bootloader SDK state */
    bootParams.timeout          = paramsTimeout;
    bootParams.dataBuffer       = &buffer[0];
    bootParams.packetBuffer     = &packet[0];

    status = Cy_Bootload_Init(&state, &bootParams);

    /* Ensure Bootloader Metadata is valid */
    status = HandleMetadata(&bootParams);
    if (status != CY_BOOTLOAD_SUCCESS)
    {
        Cy_SysLib_Halt(0x00u);
    }
    
    /*
    * In the case of non-software reset and user does not
    * want to stay in bootloader check if there is a valid app image.
    * If there is - switch to it.
    */
    if ((Cy_SysLib_GetResetReason() != CY_SYSLIB_RESET_SOFT) && (IsButtonPressed(2000u) == false))
    {
        status = Cy_Bootload_ValidateApp(1u, &bootParams);
        if (status == CY_BOOTLOAD_SUCCESS)
        {
            /*        
            * Clear reset reason because Cy_Bootload_ExecuteApp() performs        
            * a software reset.        
            * Without clearing two reset reasons would be present.        
            */
            do
            {
                Cy_SysLib_ClearResetReason();
            }while(Cy_SysLib_GetResetReason() != 0);
            /* Never returns */
            Cy_Bootload_ExecuteApp(1u);
        }
    }
    
    /* Initialize bootloader communication */
    Cy_Bootload_TransportStart();
    /* Initializes the Immediate Alert Service */
    IasInit();
    
    /* Output current stack version to UART */
    apiResult = Cy_BLE_GetStackLibraryVersion(&stackVersion);
    if(apiResult != CY_BLE_SUCCESS)
    {
        DBG_PRINTF("CyBle_GetStackLibraryVersion API Error: 0x%2.2x \r\n", apiResult);
    }
    else
    {
        DBG_PRINTF("Stack Version: %d.%d.%d.%d \r\n", stackVersion.majorVersion, 
            stackVersion.minorVersion, stackVersion.patch, stackVersion.buildNumber);
    }
    
    for(;;)
    {
        /* CyBle_ProcessEvents() allows BLE stack to process pending events */
        Cy_BLE_ProcessEvents();

        /* Process bootloader commands */
        status = Cy_Bootload_Continue(&state, &bootParams);
        ++count;

        switch(state)
        {
        case CY_BOOTLOAD_STATE_FINISHED:
            /* Finished bootloading the application image */
            
            /* Validate bootloaded application, if it is valid then switch to it */
            status = Cy_Bootload_ValidateApp(1u, &bootParams);
            if (status == CY_BOOTLOAD_SUCCESS)
            {
                Cy_Bootload_TransportStop();
                /*        
                * Clear reset reason because Cy_Bootload_ExecuteApp() performs        
                * a software reset.        
                * Without clearing two reset reasons would be present.        
                */        
                do
                {
                    Cy_SysLib_ClearResetReason();
                }while(Cy_SysLib_GetResetReason() != 0);
                /* Never returns */
                Cy_Bootload_ExecuteApp(1u);
            }
            else if (status == CY_BOOTLOAD_ERROR_VERIFY)
            {
                /*
                * Restarts Bootloading, alternatives are to Halt MCU here
                * or switch to the other app if it is valid.
                * Error code may be handled here, i.e. print to debug UART.
                */
                status = Cy_Bootload_Init(&state, &bootParams);
                /* Reset LED */
                ConnectedLED();
                ledTimer = 0;
                Cy_Bootload_TransportReset();
            }
            break;
        case CY_BOOTLOAD_STATE_FAILED:
            /* Handle error here */
            DBG_PRINTF("Bootloading has failed with error code 0x%x, try again\r\n", status);
            
            /* In this Code Example just restart bootloading process */
            status = Cy_Bootload_Init(&state, &bootParams);
            /* Reset LED */
            ConnectedLED();
            ledTimer = 0;
            Cy_Bootload_TransportReset();
            break;        
        case CY_BOOTLOAD_STATE_BOOTLOADING:            
            /* Reset timeout counter, if a command was correctly received */
            if (status == CY_BOOTLOAD_SUCCESS)
            {
                count = 0u;
            }
            else if (status == CY_BOOTLOAD_ERROR_TIMEOUT)
            {
                /*
                * if no command has been received during 5 seconds when the bootloading
                * has started then restart bootloading.
                */
                if (count >= counterTimeoutSeconds(5u, paramsTimeout))
                {
                    count = 0u;
                    Cy_Bootload_Init(&state, &bootParams);
                    /* Reset LED */
                    ConnectedLED();
                    ledTimer = 0;
                    Cy_Bootload_TransportReset();
                }
            }
            else
            {
                count = 0u;
                /* Delay because Transport still may be sending error response to a host */
                Cy_SysLib_Delay(paramsTimeout);
                Cy_Bootload_Init(&state, &bootParams);
                Cy_Bootload_TransportReset();
            }
            break;
        }
        
        /* LED logic, constant values are optimized out. */
        /* Reset timer after 2 seconds */
        if(ledTimer == (2000u / paramsTimeout)) ledTimer = 0;
        /* Every 100 miliseconds */
        if(!(ledTimer % (100u / paramsTimeout)))
        {
            /* Generates two 100 miliseconds pulses, every 2 seconds */
            if((state == CY_BOOTLOAD_STATE_BOOTLOADING) && (ledTimer < (400u / paramsTimeout)))
            {
                BlinkLED();
            }
            /* Generates one 100 miliseconds pulse, every 2 seconds */
            else if ((Cy_BLE_GetAdvertisementState() == CY_BLE_ADV_STATE_ADVERTISING)
                     && (ledTimer < (200u / paramsTimeout)))
            {
                BlinkLED();            
            }
            else
            {
                /* Remain OFF */
                ConnectedLED();
            }
        }
        ++ledTimer;
        
        /* Check if a switch to the other app is requested and perform the switch if it is */
        if((buttonReleased == true) && (state == CY_BOOTLOAD_STATE_NONE))
        {
            bool switchRequested = false;
            if (alertLevel != 0)
            {
                switchRequested = true;
            }
            else if(IsButtonPressed(500u) == true)
            {
                switchRequested = true;
                buttonReleased = false;
            }
            if (switchRequested)
            {
                /* Validate and switch to App1 */
                cy_en_bootload_status_t status = Cy_Bootload_ValidateApp(1u, &bootParams);
                
                if (status == CY_BOOTLOAD_SUCCESS)
                {
                    Cy_Bootload_TransportStop();
                    /*        
                    * Clear reset reason because Cy_Bootload_ExecuteApp() performs        
                    * a software reset.        
                    * Without clearing two reset reasons would be present.        
                    */
                    do
                    {
                        Cy_SysLib_ClearResetReason();
                    }while(Cy_SysLib_GetResetReason() != 0);
                    /* Never returns */
                    Cy_Bootload_ExecuteApp(1u);
                }
            }
        }
        else
        {
            buttonReleased = Cy_GPIO_Read(PIN_SW2_PORT, PIN_SW2_NUM);
        }     
    }
}


/*******************************************************************************
* Function Name: IsButtonPressed
********************************************************************************
*  Checks if button is pressed for a 'timeoutInMilis' time.
*
* Params:
*   timeout: Amount of time to check if button was pressed. Broken into
*            20 miliseconds steps.
* Returns:
*  true if button is pressed for specified amount.
*  false otherwise.
*******************************************************************************/
static bool IsButtonPressed(uint16_t timeoutInMilis)
{
    uint16_t buttonTime = 0;
    bool buttonPressed = false;
    timeoutInMilis /= 20;
    while(Cy_GPIO_Read(PIN_SW2_PORT, PIN_SW2_NUM) == 0u)
    {
        Cy_SysLib_Delay(20u);
        if(++buttonTime == timeoutInMilis)
        {
            /* time has passed */
            buttonPressed = true;
            break;
        }
    }
    return buttonPressed;
}

/*******************************************************************************
* Function Name: counterTimeoutSeconds
********************************************************************************
* Returns number of counts that correspond to number of seconds passed as
* a parameter.
* E.g. comparing counter with 300 seconds is like this.
* ---
* uint32_t counter = 0u;
* for (;;)
* {
*     Cy_SysLib_Delay(UART_TIMEOUT);
*     ++count;
*     if (count >= counterTimeoutSeconds(seconds: 300u, timeout: UART_TIMEOUT))
*     {
*         count = 0u;
*         DoSomething();
*     }
* }
* ---
*
* Both parameters are required to be compile time constants,
* so this function gets optimized out to single constant value.
*
* Parameters:
*  seconds    Number of seconds to pass. Must be less that 4_294_967 seconds.
*  timeout    Timeout for Cy_Bootload_Continue() function, in milliseconds.
*             Must be greater than zero.
*             It is recommended to be a value that produces no reminder
*             for this function to be precise.
* Return:
*  See description.
*******************************************************************************/
static uint32_t counterTimeoutSeconds(uint32_t seconds, uint32_t timeout)
{
    return (seconds * 1000ul) / timeout;
}

/*******************************************************************************
* Function Name: CopyRow
********************************************************************************
* Copies data from a "src" address to a flash row with the address "dest".
* If "src" data is the same as "dest" data then no copy is needed.
*
* Parameters:
*  dest     Destination address. Has to be an address of the start of flash row.
*  src      Source address. Has to be properly aligned.
*  rowSize  Size of flash row.
*
* Returns:
*  CY_BOOTLAOD_SUCCESS if operation is successful.
*  Error code in a case of failure.
*******************************************************************************/
static cy_en_bootload_status_t CopyRow(uint32_t dest, uint32_t src, uint32_t rowSize, cy_stc_bootload_params_t * params)
{
    cy_en_bootload_status_t status;
    
    /* Save params->dataBuffer value */
    uint8_t *buffer = params->dataBuffer;

    /* Compare "dest" and "src" content */
    params->dataBuffer = (uint8_t *)src;
    status = Cy_Bootload_ReadData(dest, rowSize, CY_BOOTLOAD_IOCTL_COMPARE, params);
    
    /* Restore params->dataBuffer */
    params->dataBuffer = buffer;

    /* If "dest" differs from "src" then copy "src" to "dest" */
    if (status != CY_BOOTLOAD_SUCCESS)
    {
        (void) memcpy((void *) params->dataBuffer, (const void*)src, rowSize);
        status = Cy_Bootload_WriteData(dest, rowSize, CY_BOOTLOAD_IOCTL_WRITE, params);
    }
    /* Restore params->dataBuffer */
    params->dataBuffer = buffer;
    
    return (status);
}

/*******************************************************************************
* Function Name: HandleMetadata
********************************************************************************
* The goal of this function is to make Bootloader SDK metadata (MD) valid.
* The following algorithm is used (in C-like pseudocode):
* ---
* if (isValid(MD) == true)
* {   if (MDC != MD)
*         MDC = MD;
* } else
* {   if(isValid(MDC) )
*         MD = MDC;
*     #if MD Writeable
*     else
*         MD = INITIAL_VALUE;
*     #endif
* }
* ---
* Here MD is metadata flash row, MDC is flash row with metadata copy,
* INITIAL_VALUE is known initial value.
*
* In this code example MDC is placed in the next flash row after the MD, and
* INITIAL_VALUE is MD with only CRC, App0 start and size initialized,
* all the other fields are not touched. This is only done if metadata is
* writeable when bootloading.
*
* Parameters:
*  params   A pointer to a Bootloader SDK parameters structure.
*
* Returns:
* - CY_BOOTLOAD_SUCCESS when finished normally.
* - Any other status code on error.
*******************************************************************************/
static cy_en_bootload_status_t HandleMetadata(cy_stc_bootload_params_t *params)
{
    const uint32_t MD     = (uint32_t)(&__cy_boot_metadata_addr   ); /* MD address  */
    const uint32_t mdSize = (uint32_t)(&__cy_boot_metadata_length ); /* MD size, assumed to be one flash row */
    const uint32_t MDC    = MD + mdSize;                             /* MDC address */

    cy_en_bootload_status_t status = CY_BOOTLOAD_SUCCESS;
    
    status = Cy_Bootload_ValidateMetadata(MD, params);
    if (status == CY_BOOTLOAD_SUCCESS)
    {
        /* Checks if MDC equals to DC, if no then copies MD to MDC */
        status = CopyRow(MDC, MD, mdSize, params);
    }
    else
    {
        status = Cy_Bootload_ValidateMetadata(MDC, params);
        if (status == CY_BOOTLOAD_SUCCESS)
        {
            /* Copy MDC to MD */
            status = CopyRow(MD, MDC, mdSize, params);
        }
        #if CY_BOOTLOAD_METADATA_WRITABLE != 0
        if (status != CY_BOOTLOAD_SUCCESS)
        {
            const uint32_t elfStartAddress = 0x10000000;
            const uint32_t elfAppSize      = 0x40000;
            /* Set MD to INITIAL_VALUE */
            status = Cy_Bootload_SetAppMetadata(0u, elfStartAddress, elfAppSize, params);
        }
        #endif /* CY_BOOTLOAD_METADATA_WRITABLE != 0 */
    }
    return (status);
}


/*******************************************************************************
* Function Name: AppCallBack()
********************************************************************************
*
* Summary:
*   This is an event callback function to receive events from the BLE Component.
*   Used in Cy_Bootload_TransportStart()
*
*  event - the event code
*  *eventParam - the event parameters
*
*******************************************************************************/
void AppCallBack(uint32 event, void* eventParam)
{
    cy_en_ble_api_result_t apiResult;
    
    static cy_stc_ble_gap_sec_key_info_t keyInfo =
    {
        .localKeysFlag = CY_BLE_GAP_SMP_INIT_ENC_KEY_DIST | 
                         CY_BLE_GAP_SMP_INIT_IRK_KEY_DIST | 
                         CY_BLE_GAP_SMP_INIT_CSRK_KEY_DIST,
        .exchangeKeysFlag = CY_BLE_GAP_SMP_INIT_ENC_KEY_DIST | 
                            CY_BLE_GAP_SMP_INIT_IRK_KEY_DIST | 
                            CY_BLE_GAP_SMP_INIT_CSRK_KEY_DIST |
                            CY_BLE_GAP_SMP_RESP_ENC_KEY_DIST |
                            CY_BLE_GAP_SMP_RESP_IRK_KEY_DIST |
                            CY_BLE_GAP_SMP_RESP_CSRK_KEY_DIST,
    };
        
    switch (event)
    {
    /**********************************************************
    *                       General Events
    ***********************************************************/
    
    /* This event received when BLE communication starts */
    case CY_BLE_EVT_STACK_ON:
        /* Enter into discoverable mode so that remote can search it. */
        apiResult = Cy_BLE_GAPP_StartAdvertisement(CY_BLE_ADVERTISING_FAST, 0u);
        if(apiResult != CY_BLE_SUCCESS)
        {
        }
        
        apiResult = Cy_BLE_GAP_GenerateKeys(&keyInfo);
        if(apiResult != CY_BLE_SUCCESS)
        {
            DBG_PRINTF("CyBle_GapGenerateKeys API Error: %d \r\n", apiResult);
        }
        break;

    /* This event indicates that some internal HW error has occurred. */
    case CY_BLE_EVT_HARDWARE_ERROR:
        DBG_PRINTF("CYBLE_EVT_HARDWARE_ERROR\r\n");
        break;
    /**********************************************************
    *                       GAP Events
    ***********************************************************/
    case CY_BLE_EVT_GAP_AUTH_REQ:
        DBG_PRINTF("CYBLE_EVT_AUTH_REQ: security=%x, bonding=%x, ekeySize=%x, err=%x \r\n",
            (*(cy_stc_ble_gap_auth_info_t *)eventParam).security,
            (*(cy_stc_ble_gap_auth_info_t *)eventParam).bonding,
            (*(cy_stc_ble_gap_auth_info_t*)eventParam).ekeySize,
            (*(cy_stc_ble_gap_auth_info_t *)eventParam).authErr    );
        if ( cy_ble_configPtr->authInfo[CY_BLE_SECURITY_CONFIGURATION_0_INDEX].security 
             == (CY_BLE_GAP_SEC_MODE_1 | CY_BLE_GAP_SEC_LEVEL_1)  )
        {
            cy_ble_configPtr->authInfo[CY_BLE_SECURITY_CONFIGURATION_0_INDEX].authErr =
                CY_BLE_GAP_AUTH_ERROR_PAIRING_NOT_SUPPORTED;
        }    
        
        cy_ble_configPtr->authInfo[CY_BLE_SECURITY_CONFIGURATION_0_INDEX].bdHandle =
            ((cy_stc_ble_gap_auth_info_t *)eventParam)->bdHandle;

        apiResult = Cy_BLE_GAPP_AuthReqReply(&cy_ble_configPtr->authInfo[CY_BLE_SECURITY_CONFIGURATION_0_INDEX]);
        if(apiResult != CY_BLE_SUCCESS)
        {
            Cy_BLE_GAP_RemoveOldestDeviceFromBondedList();
            apiResult = Cy_BLE_GAPP_AuthReqReply(&cy_ble_configPtr->authInfo[CY_BLE_SECURITY_CONFIGURATION_0_INDEX]);
            if(apiResult != CY_BLE_SUCCESS)
            {
               DBG_PRINTF("CyBle_GappAuthReqReply API Error: %d \r\n", apiResult);
            }
        }
        break;

    case CY_BLE_EVT_GAP_PASSKEY_ENTRY_REQUEST:
        DBG_PRINTF("CYBLE_EVT_PASSKEY_ENTRY_REQUEST press 'p' to enter passkey \r\n");
        break;

    case CY_BLE_EVT_GAP_PASSKEY_DISPLAY_REQUEST:
        DBG_PRINTF("CYBLE_EVT_PASSKEY_DISPLAY_REQUEST %6.6d \r\n", *(int *)eventParam);
        break;
    
    case CY_BLE_EVT_GAP_KEYINFO_EXCHNGE_CMPLT:
        DBG_PRINTF("CYBLE_EVT_GAP_KEYINFO_EXCHNGE_CMPLT \r\n");
        break;
    
    case CY_BLE_EVT_GAP_AUTH_COMPLETE:
        DBG_PRINTF("AUTH_COMPLETE \r\n");
        break;
    
    case CY_BLE_EVT_GAP_AUTH_FAILED:
        DBG_PRINTF("CYBLE_EVT_AUTH_FAILED: %x \r\n", *(uint8 *)eventParam);
        break;

    case CY_BLE_EVT_GAP_DEVICE_CONNECTED:
        DBG_PRINTF("CYBLE_EVT_GAP_DEVICE_CONNECTED: %d \r\n", appConnHandle.bdHandle);
        
        if ( ((*(cy_stc_ble_gap_connected_param_t *)eventParam).connIntv 
             < CYBLE_GAPP_CONNECTION_INTERVAL_MIN ) || (
             (*(cy_stc_ble_gap_connected_param_t *)eventParam).connIntv 
             > CYBLE_GAPP_CONNECTION_INTERVAL_MAX ) )
        {
            cy_stc_ble_gap_conn_update_param_info_t connUpdateParam;
            /* If connection settings do not match expected ones - request parameter update */
            connUpdateParam.connIntvMin   = CYBLE_GAPP_CONNECTION_INTERVAL_MIN;
            connUpdateParam.connIntvMax   = CYBLE_GAPP_CONNECTION_INTERVAL_MAX;
            connUpdateParam.connLatency   = CYBLE_GAPP_CONNECTION_SLAVE_LATENCY;
            connUpdateParam.supervisionTO = CYBLE_GAPP_CONNECTION_TIME_OUT;
            connUpdateParam.bdHandle = appConnHandle.bdHandle;
            apiResult = Cy_BLE_L2CAP_LeConnectionParamUpdateRequest(&connUpdateParam);
            DBG_PRINTF("Cy_BLE_L2CAP_LeConnectionParamUpdateRequest API: 0x%2.2x \r\n", apiResult);
        }        
        keyInfo.SecKeyParam.bdHandle = (*(cy_stc_ble_gap_connected_param_t *)eventParam).bdHandle;
        apiResult = Cy_BLE_GAP_SetSecurityKeys(&keyInfo);
        if(apiResult != CY_BLE_SUCCESS)
        {
            DBG_PRINTF("CyBle_GapSetSecurityKeys API Error: %d \r\n", apiResult);
        }
        break;
        
    case CY_BLE_EVT_L2CAP_CONN_PARAM_UPDATE_RSP:
        DBG_PRINTF("CY_BLE_EVT_L2CAP_CONN_PARAM_UPDATE_RSP, result = %d\r\n", 
        (*(cy_stc_ble_l2cap_conn_update_rsp_param_t *)eventParam).result);
        break; 

    case CY_BLE_EVT_GAP_KEYS_GEN_COMPLETE:
        DBG_PRINTF("CYBLE_EVT_GAP_KEYS_GEN_COMPLETE \r\n");
        keyInfo.SecKeyParam = (*(cy_stc_ble_gap_sec_key_param_t *)eventParam);
        Cy_BLE_GAP_SetIdAddress(&cy_ble_deviceAddress);
        break;
    
    case CY_BLE_EVT_GAP_DEVICE_DISCONNECTED:
        DBG_PRINTF("CYBLE_EVT_GAP_DEVICE_DISCONNECTED\r\n");
        /* Put the device into discoverable mode so that a remote can search it. */
        apiResult = Cy_BLE_GAPP_StartAdvertisement(CY_BLE_ADVERTISING_FAST, CY_BLE_PERIPHERAL_CONFIGURATION_0_INDEX);
        if(apiResult != CY_BLE_SUCCESS)
        {
            DBG_PRINTF("StartAdvertisement API Error: %d \r\n", apiResult);
        }
        break;
    
    case CY_BLE_EVT_GAP_ENCRYPT_CHANGE:
        DBG_PRINTF("CYBLE_EVT_GAP_ENCRYPT_CHANGE: %x \r\n", *(uint8 *)eventParam);
        break;
    
    case CY_BLE_EVT_GAP_CONNECTION_UPDATE_COMPLETE:
        DBG_PRINTF("CYBLE_EVT_CONNECTION_UPDATE_COMPLETE: %x \r\n", *(uint8 *)eventParam);
        break;
    
    case CY_BLE_EVT_GAPP_ADVERTISEMENT_START_STOP:
        if(Cy_BLE_GetAdvertisementState() == CY_BLE_ADV_STATE_STOPPED)
        {   
            /* Fast and slow advertising period complete, go to low power  
             * mode (Hibernate mode) and wait for an external
             * user event to wake up the device again */
            
            /* Stop bootloading communication */
            Cy_Bootload_TransportStop();
            /* Check if app is valid, if it is then switch to it */
            uint32_t status = Cy_Bootload_ValidateApp(1u, NULL);
            if (status == CY_BOOTLOAD_SUCCESS)
            {
                /*        
                * Clear reset reason because Cy_Bootload_ExecuteApp() performs        
                * a software reset.        
                * Without clearing two reset reasons would be present.        
                */
                do
                {
                    Cy_SysLib_ClearResetReason();
                }while(Cy_SysLib_GetResetReason() != 0);
                /* Never returns */
                Cy_Bootload_ExecuteApp(1u);
            }
            /* 300 seconds has passed and App is invalid. Hibernate */
            HibernateLED();
            Cy_SysPm_Hibernate();
        }
        break;

    /**********************************************************
    *                       GATT Events
    ***********************************************************/
    case CY_BLE_EVT_GATT_CONNECT_IND:
        appConnHandle = *(cy_stc_ble_conn_handle_t *)eventParam;
        DBG_PRINTF("CYBLE_EVT_GATT_CONNECT_IND: %d \r\n", appConnHandle.bdHandle);
        break;
    
    case CY_BLE_EVT_GATT_DISCONNECT_IND:
        DBG_PRINTF("CYBLE_EVT_GATT_DISCONNECT_IND: %d \r\n", ((cy_stc_ble_conn_handle_t *)eventParam)->bdHandle);
        break;
    
    case CY_BLE_EVT_GATTS_WRITE_CMD_REQ:
        DBG_PRINTF("CYBLE_EVT_GATTS_WRITE_CMD_REQ\r\n");
        break;

    default:
        break;
    }
}


/* [] END OF FILE */
