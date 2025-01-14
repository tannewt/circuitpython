#include "supervisor/usb.h"

#include <stm32_ll_pwr.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>

#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32_otghs)
#define UDC_IRQ_NAME     otghs
#elif DT_HAS_COMPAT_STATUS_OKAY(st_stm32_otgfs)
#define UDC_IRQ_NAME     otgfs
#elif DT_HAS_COMPAT_STATUS_OKAY(st_stm32_usb)
#define UDC_IRQ_NAME     usb
#endif

#define USB_DEVICE DT_NODELABEL(zephyr_udc0)

#define UDC_IRQ       DT_IRQ_BY_NAME(USB_DEVICE, UDC_IRQ_NAME, irq)
#define UDC_IRQ_PRI   DT_IRQ_BY_NAME(USB_DEVICE, UDC_IRQ_NAME, priority)

PINCTRL_DT_DEFINE(USB_DEVICE);
static const struct pinctrl_dev_config *usb_pcfg =
    PINCTRL_DT_DEV_CONFIG_GET(USB_DEVICE);

void init_usb_hardware(void) {
    IRQ_CONNECT(UDC_IRQ, UDC_IRQ_PRI, usb_irq_handler, 0, 0);

    /* Configure USB GPIOs */
    int err = pinctrl_apply_state(usb_pcfg, PINCTRL_STATE_DEFAULT);
    if (err < 0) {
        printk("USB pinctrl setup failed (%d)\n", err);
    } else {
        printk("USB pins setup\n");
    }

// #ifdef USB_DRD_FS
//   // STM32U535/STM32U545

//   /* Enable USB power on Pwrctrl CR2 register */
//   HAL_PWREx_EnableVddUSB();

//   /* USB clock enable */
//   __HAL_RCC_USB_FS_CLK_ENABLE();

// #endif

    #ifdef USB_OTG_FS
    /* Enable USB power on Pwrctrl CR2 register */
    // HAL_PWREx_EnableVddUSB();
    LL_PWR_EnableVddUSB();

    /* USB clock enable */
    __HAL_RCC_USB_OTG_FS_CLK_ENABLE();

    #endif

// #ifdef USB_OTG_HS
//   // STM59x/Ax/Fx/Gx only have 1 USB HS port

//   #if CFG_TUSB_OS == OPT_OS_FREERTOS
//   // If freeRTOS is used, IRQ priority is limit by max syscall ( smaller is higher )
//   NVIC_SetPriority(OTG_HS_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
//   #endif

//   /* USB clock enable */
//   __HAL_RCC_USB_OTG_HS_CLK_ENABLE();
//   __HAL_RCC_USBPHYC_CLK_ENABLE();

//   /* Enable USB power on Pwrctrl CR2 register */
//   HAL_PWREx_EnableVddUSB();
//   HAL_PWREx_EnableUSBHSTranceiverSupply();

//   /*Configuring the SYSCFG registers OTG_HS PHY*/
//   HAL_SYSCFG_EnableOTGPHY(SYSCFG_OTG_HS_PHY_ENABLE);

//   // Disable VBUS sense (B device)
//   USB_OTG_HS->GCCFG &= ~USB_OTG_GCCFG_VBDEN;

//   // B-peripheral session valid override enable
//   USB_OTG_HS->GCCFG |= USB_OTG_GCCFG_VBVALEXTOEN;
//   USB_OTG_HS->GCCFG |= USB_OTG_GCCFG_VBVALOVAL;
// #endif // USB_OTG_FS

}
