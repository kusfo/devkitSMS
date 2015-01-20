/* *********************************************
   SMSlib - C programming library for the SMS
   ********************************************* */

#include <stdbool.h>
#include "SMSlib.h"

/* define VDPControlPort (SDCC z80 syntax) */   
__sfr __at 0xBF VDPControlPort;
/* define VDPStatusPort */
__sfr __at 0xBF VDPStatusPort;
/* define VDPDataPort */
__sfr __at 0xBE VDPDataPort;
/* define IOPort (joypad) */
__sfr __at 0xDC IOPortL;
__sfr __at 0xDE IOPortH;

#define HI(x)                 ((x)>>8)
#define LO(x)                 ((x)&0xFF)

#define MAXSPRITES            64

/*
#define DISABLE_INTERRUPTS    __asm di __endasm
#define ENABLE_INTERRUPTS     __asm ei __endasm
*/

unsigned int PNTAddress=0x3800;
unsigned int SATAddress=0x3F00;

/* the VDP registers 'shadow' RAM */
unsigned char VDPReg[0x0B]= { 0x04, /* reg0: Mode 4 */
                              0x00, /* reg1: display and frame int OFF */
                              0xFF, /* reg2: PNT at 0x3800 */
                              0xFF, /* reg3: no effect (when in mode 4) */
                              0xFF, /* reg4: no effect (when in mode 4) */
                              0xFF, /* reg5: SAT at 0x3F00 */
                              0xFF, /* reg6: Sprite tiles at 0x2000 */
                              0x00, /* reg7: backdrop color (zero) */
                              0x00, /* reg8: scroll X (zero) */
                              0x00, /* reg9: scroll Y (zero) */
                              0xFF  /* regA: line interrupt (disabled) */
                             };

volatile bool VDPBlank=false;               /* used by INTerrupt */
volatile bool VDPSpriteOverflow=false;
volatile bool VDPSpriteCollision=false;
volatile unsigned char VDPStatus=0;
volatile unsigned int KeysStatus=0;
volatile unsigned int PreviousKeysStatus=0;
volatile unsigned int KeysPressed=0;
volatile unsigned int KeysHeld=0;
volatile unsigned int KeysReleased=0;
unsigned char clipWin_x0,clipWin_y0,clipWin_x1,clipWin_y1;

volatile bool PauseRequested=false;         /* used by NMI */

unsigned char SpriteTableY[MAXSPRITES];
unsigned char SpriteTableXN[MAXSPRITES*2];
unsigned char SpriteNextFree;

/* macro instead of inline functions. I had no choice :| */
#define SMS_write_to_VDPRegister(VDPReg,value)    __critical { VDPControlPort=value; VDPControlPort=VDPReg|0x80; }
#define SMS_set_address_CRAM(address)             __critical { VDPControlPort=address; VDPControlPort=0xC0; }
#define SMS_set_address_VRAM(address)             __critical { VDPControlPort=LO(address); VDPControlPort=HI(address)|0x40; }

inline void SMS_byte_to_VDP_data (unsigned char data) {
  /* INTERNAL FUNCTION */
  VDPDataPort=data;
}

inline void SMS_byte_array_to_VDP_data (const unsigned char *data, unsigned int len) {
  /* INTERNAL FUNCTION */
  while (len--!=0)
    VDPDataPort=*(data++);
}

inline void SMS_word_to_VDP_data (unsigned int data) {
  /* INTERNAL FUNCTION */
  VDPDataPort=LO(data);
  VDPDataPort=HI(data);
}

inline void SMS_word_array_to_VDP_data (const unsigned int *data, unsigned int len) {
  /* INTERNAL FUNCTION */
  while (len--!=0) {
    VDPDataPort=LO(*(data));
    VDPDataPort=HI(*(data));
    data++;
  }
}

void SMS_init (void) {
  /* Initializes the lib */
  int i;
  /* VDP initialization */
  for (i=0;i<0x0B;i++)
    SMS_write_to_VDPRegister(i,VDPReg[i]);
  /* init Pause */
  SMS_resetPauseRequest();
  /* SMS_setClippingWindow(0,0,256,192);  */
}

void SMS_VDPturnOnFeature (unsigned int feature) {
  /* turns on a VDP feature */
  VDPReg[HI(feature)]|=LO(feature);
  SMS_write_to_VDPRegister (HI(feature),VDPReg[HI(feature)]);
}

void SMS_VDPturnOffFeature (unsigned int feature) {
  /* turns off a VDP feature */
  VDPReg[HI(feature)]&=~LO(feature);
  SMS_write_to_VDPRegister (HI(feature),VDPReg[HI(feature)]);
}

void SMS_setBGPaletteColor (unsigned char entry, unsigned char color) {
  SMS_set_address_CRAM(entry);
  SMS_byte_to_VDP_data(color);
}

void SMS_setSpritePaletteColor (unsigned char entry, unsigned char color) {
  SMS_set_address_CRAM(entry+0x10);
  SMS_byte_to_VDP_data(color);
}

void SMS_loadBGPalette (const unsigned char *palette) {
  SMS_set_address_CRAM(0x00);
  SMS_byte_array_to_VDP_data(palette,16);
}

void SMS_loadSpritePalette (const unsigned char *palette) {
  SMS_set_address_CRAM(0x10);
  SMS_byte_array_to_VDP_data(palette,16);
}

void SMS_loadTiles (unsigned char *src, unsigned int Tilefrom, unsigned int len) {
  SMS_set_address_VRAM(Tilefrom*32);
  SMS_byte_array_to_VDP_data(src,len*32);
}

void SMS_setTileatXY (unsigned char x, unsigned char y, unsigned int tile) {
  SMS_set_address_VRAM(PNTAddress+(y*32+x)*2);
  SMS_word_to_VDP_data(tile,2);
}

void SMS_loadTileMap (unsigned char x, unsigned char y, unsigned int *src, unsigned int len) {
  SMS_set_address_VRAM(PNTAddress+(y*32+x)*2);
  SMS_word_array_to_VDP_data(src,len);
}

void SMS_loadTileMapArea (unsigned char x, unsigned char y,  unsigned int *src, unsigned char width, unsigned char height) {
  unsigned char cur_y;
  for (cur_y=y;cur_y<y+height;cur_y++) {
    SMS_set_address_VRAM(PNTAddress+(cur_y*32+x)*2);
    SMS_word_array_to_VDP_data(src,width);
    src+=width;
  }
}

void SMS_initSprites (void) {
  SpriteNextFree=0;
}

bool SMS_addSprite (unsigned char x, int y, unsigned char tile) {
  if (SpriteNextFree<MAXSPRITES) {
    SpriteTableY[SpriteNextFree]=LO(y-1);
    SpriteTableXN[SpriteNextFree*2]=x;
    SpriteTableXN[SpriteNextFree*2+1]=tile;
    SpriteNextFree++;
    return (true);
  } else
    return (false);
}

/*
void SMS_setClippingWindow (unsigned char x0, unsigned char y0, unsigned char x1, unsigned char y1) {
  clipWin_x0=x0;
  clipWin_y0=y0;
  clipWin_x1=x1;
  clipWin_y1=y1;
}

bool SMS_addSpriteClipping (int x, int y, unsigned char tile) {
  if (SpriteNextFree<MAXSPRITES) {
    if ((x>(clipWin_x1-8))||(x<clipWin_x0))
      return (false);                               // sprite clipped
    y--;
    if ((y>(clipWin_y1-8))||(y<clipWin_y0))
      return (false);                               // sprite clipped
    SpriteTableY[SpriteNextFree]=LO(y);
    SpriteTableXN[SpriteNextFree*2]=x;
    SpriteTableXN[SpriteNextFree*2+1]=tile;
    SpriteNextFree++;
    return (true);
  } else
    return (false);
}
*/

void SMS_finalizeSprites (void) {
  if (SpriteNextFree<64)
    SpriteTableY[SpriteNextFree]=0xD0;
}

void SMS_copySpritestoSAT (void) {
  SMS_set_address_VRAM(SATAddress);
  SMS_byte_array_to_VDP_data(SpriteTableY,MAXSPRITES);
  SMS_set_address_VRAM(SATAddress+128);
  SMS_byte_array_to_VDP_data(SpriteTableXN,MAXSPRITES*2);
}

void SMS_waitForVBlank (void) {
  VDPBlank=false;
  while (!VDPBlank);
}

unsigned int SMS_getKeysStatus (void) {
  return (KeysStatus);
}

unsigned int SMS_getKeysPressed (void) {
  return (KeysPressed);
}

unsigned int SMS_getKeysHeld (void) {
  return (KeysHeld);
}

unsigned int SMS_getKeysReleased (void) {
  return (KeysReleased);
}

bool SMS_queryPauseRequested (void) {
  return(PauseRequested);
}

void SMS_resetPauseRequest (void) {
  PauseRequested=false;
}

/* Interrupt Service Routines */
void SMS_isr (void) __interrupt {
  VDPStatus=VDPStatusPort;            /* this also aknowledge interrupt at VDP */

  if (VDPStatus & 0x80) {
    VDPBlank=true;
    VDPSpriteOverflow=(VDPStatus & 0x40);
    VDPSpriteCollision=(VDPStatus & 0x20);

    /* read key input */
    PreviousKeysStatus=KeysStatus;
    KeysStatus=((~IOPortH)<<8)|(~IOPortL);
    KeysHeld=KeysStatus&PreviousKeysStatus;
    KeysPressed=KeysStatus&(~PreviousKeysStatus);
    KeysReleased=(~KeysStatus)&PreviousKeysStatus;
  }  /* else
    LineInterruptHandler(); */        /* TO DO */

  /* Z80 disable the interrupts on ISR, so we should re-enable them explicitly */
  __asm
    ei
  __endasm;
}

void SMS_nmi_isr (void) __critical __interrupt {          /* this is for NMI */
  PauseRequested=true;
}


/* *********** TEMPORARY CODE (needs fixing) ************************ */

/*
void SMS_VDPSetSATLocation (unsigned int location) {
  VDPReg[5]=(location>>7)|0b10000001;
  SMS_write_to_VDPRegister (5,VDPReg[5]);
}

void SMS_VDPSetPNTLocation (unsigned int location) {
  VDPReg[2]=(location>>10)|0b11110001;
  SMS_write_to_VDPRegister (2,VDPReg[2]);
}

void SMS_VDPSetSpritesLocation (unsigned int location) {
  VDPReg[6]=(location>>11)|0b11111011;
  SMS_write_to_VDPRegister (6,VDPReg[6]);
}
*/

/* *********** END ************************ */


/* inline critical function doesn't work correctly */
/*
inline void SMS_write_to_VDPRegister (unsigned char VDPReg, unsigned char value) {
   INTERNAL FUNCTION
  __critical {
    VDPControlPort=value;
    VDPControlPort=VDPReg|0x80;
  }
}

inline void SMS_set_address_CRAM (unsigned char address) {
   INTERNAL FUNCTION
  __critical {
    VDPControlPort=address;
    VDPControlPort=0xC0;
  }
}

inline void SMS_set_address_VRAM (unsigned int address) {
   INTERNAL FUNCTION
  __critical {
    VDPControlPort=LO(address);
    VDPControlPort=HI(address)|0x40;
  }
}
*/




/* *********** OLD CODE (that won't probably be used) ************************ */


/*
void SMS_VDPToggleFeature (unsigned int feature, bool on) {

  if (on)
    VDPReg[HI(feature)]|=LO(feature);
  else
    VDPReg[HI(feature)]&=~LO(feature); 
  SMS_write_to_VDPRegister (HI(feature),VDPReg[HI(feature)]);
}
*/
