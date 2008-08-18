/* FCE Ultra - NES/Famicom Emulator
* 
* Copyright notice for this file:
*  Copyright (C) 1998 BERO
*  Copyright (C) 2003 Xodnizel
*  
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or   
* (at your option) any later version.
* 
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of 
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the 
* GNU General Public License for more details.
* 
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include  <string.h>
#include  <stdio.h>
#include  <stdlib.h>

#include  "types.h"
#include  "x6502.h"
#include  "fceu.h"
#include  "ppu.h"
#include  "nsf.h"
#include  "sound.h"
#include  "file.h"
#include  "utils/endian.h"
#include  "utils/memory.h"

#include  "cart.h"
#include  "palette.h"
#include  "state.h"
#include  "video.h"
#include  "input.h"
#include "driver.h"

#define VBlankON  (PPU[0]&0x80)   //Generate VBlank NMI
#define Sprite16  (PPU[0]&0x20)   //Sprites 8x16/8x8 
#define BGAdrHI   (PPU[0]&0x10)   //BG pattern adr $0000/$1000
#define SpAdrHI   (PPU[0]&0x08)   //Sprite pattern adr $0000/$1000
#define INC32     (PPU[0]&0x04)   //auto increment 1/32

#define SpriteON  (PPU[1]&0x10)   //Show Sprite
#define ScreenON  (PPU[1]&0x08)   //Show screen
#define PPUON    (PPU[1]&0x18)		//PPU should operate

#define SpriteLeft8 (PPU[1]&0x04)
#define BGLeft8 (PPU[1]&0x02)

#define PPU_status      (PPU[2])

#define Pal     (PALRAM)   

static void FetchSpriteData(void);
static void RefreshLine(int lastpixel);
static void RefreshSprites(void);
static void CopySprites(uint8 *target);

static void Fixit1(void);
static uint32 ppulut1[256];
static uint32 ppulut2[256];
static uint32 ppulut3[128];


template<typename T, int BITS>
struct BITREVLUT {

	T* lut;
	BITREVLUT() {
		int bits = BITS;
		int n = 1<<BITS;
		lut = new T[n];

		int m = 1;
		int a = n>>1;
		int j = 2;

		lut[0] = 0;
		lut[1] = a;

		while(--bits) {
			m <<= 1;
			a >>= 1;
			for(int i=0;i<m;i++)
				lut[j++] = lut[i] + a;
		}
	}

	T operator[](int index) { return lut[index]; }

};
BITREVLUT<uint8,8> bitrevlut;

//uses the internal counters concept at http://nesdev.icequake.net/PPU%20addressing.txt
struct PPUREGS {
	uint32 fv;//3
	uint32 v;//1
	uint32 h;//1
	uint32 vt;//5
	uint32 ht;//5
	uint32 fh;//3
	uint32 s;//1
	uint32 par;//8
	uint32 ar;//2

	uint32 _fv, _v, _h, _vt, _ht;

	PPUREGS()
		: fv(0), v(0), h(0), vt(0), ht(0), fh(0), s(0), par(0), ar(0)
		,  _fv(0), _v(0), _h(0), _vt(0), _ht(0)
	{}

	void install_latches() {
		fv = _fv;
		v = _v;
		h = _h;
		vt = _vt;
		ht = _ht;
	}

	void install_h_latches() {
		if(ht!=_ht || h != _h) {
			int zzz=9;
		}
		ht = _ht;
		h = _h;
	}

	void clear_latches() {
		_fv = _v = _h = _vt = _ht = 0;
		fh = 0;
	}

	void increment_hsc() {
		//The first one, the horizontal scroll counter, consists of 6 bits, and is 
		//made up by daisy-chaining the HT counter to the H counter. The HT counter is 
		//then clocked every 8 pixel dot clocks (or every 8/3 CPU clock cycles).
		ht++;
		h += (ht>>5);
		ht &= 31;
		h &= 1;
	}

	void increment_vs() {
		fv++;
		vt += (fv>>3);
		v += (vt==30)?1:0;
		fv &= 7;
		if(vt==30) vt=0;
		v &= 1;
	}

	uint32 get_ntread() {
		return 0x2000 | (v<<0xB) | (h<<0xA) | (vt<<5) | ht;
	}

	uint32 get_2007access() {
		return ((fv&3)<<0xC) | (v<<0xB) | (h<<0xA) | (vt<<5) | ht;
	}
	
	//The PPU has an internal 4-position, 2-bit shifter, which it uses for 
	//obtaining the 2-bit palette select data during an attribute table byte 
	//fetch. To represent how this data is shifted in the diagram, letters a..c 
	//are used in the diagram to represent the right-shift position amount to 
	//apply to the data read from the attribute data (a is always 0). This is why 
	//you only see bits 0 and 1 used off the read attribute data in the diagram.
	uint32 get_atread() {
		return 0x2000 | (v<<0xB) | (h<<0xA) | 0x3C0 | ((vt&0x1C)<<1) | ((ht&0x1C)>>2);
	}

	//address line 3 relates to the pattern table fetch occuring (the PPU always makes them in pairs).
	uint32 get_ptread() {
		return (s<<0xC) | (par<<0x4) | fv;
	}

	void increment2007(bool by32) {
	
		//If the VRAM address increment bit (2000.2) is clear (inc. amt. = 1), all the 
		//scroll counters are daisy-chained (in the order of HT, VT, H, V, FV) so that 
		//the carry out of each counter controls the next counter's clock rate. The 
		//result is that all 5 counters function as a single 15-bit one. Any access to 
		//2007 clocks the HT counter here.
		//
		//If the VRAM address increment bit is set (inc. amt. = 32), the only 
		//difference is that the HT counter is no longer being clocked, and the VT 
		//counter is now being clocked by access to 2007.
		if(by32) {
			vt++;
		} else {
			ht++;
			vt+=(ht>>5)&1;
		}
		h+=(vt>>5);
		v+=(h>>1);
		fv+=(v>>1);
		ht &= 31;
		vt &= 31;
		h &= 1;
		v &= 1;
		fv &= 7;
	}
} ppur;

static void makeppulut(void)
{
	int x;
	int y;
	int cc,xo,pixel;


	for(x=0;x<256;x++)
	{
		ppulut1[x] = 0;

		for(y=0;y<8;y++)
		{
			ppulut1[x] |= ((x>>(7-y))&1)<<(y*4);
		}

		ppulut2[x] = ppulut1[x] << 1;
	}

	for(cc=0;cc<16;cc++)
	{
		for(xo=0;xo<8;xo++)
		{
			ppulut3[ xo | ( cc << 3 ) ] = 0;

			for(pixel=0;pixel<8;pixel++)
			{
				int shiftr;
				shiftr = ( pixel + xo ) / 8;
				shiftr *= 2;
				ppulut3[ xo | (cc<<3) ] |= ( ( cc >> shiftr ) & 3 ) << ( 2 + pixel * 4 );
			}
			//    printf("%08x\n",ppulut3[xo|(cc<<3)]);
		}
	}
} 

static int ppudead=1;
static int kook=0;
int fceuindbg=0;

//mbg 6/23/08
//make the no-bg fill color configurable
//0xFF shall indicate to use palette[0]
uint8 gNoBGFillColor = 0xFF;

int MMC5Hack=0;
uint32 MMC5HackVROMMask=0;
uint8 *MMC5HackExNTARAMPtr=0;
uint8 *MMC5HackVROMPTR=0;
uint8 MMC5HackCHRMode=0;
uint8 MMC5HackSPMode=0;   
uint8 MMC5HackSPScroll=0; 
uint8 MMC5HackSPPage=0;


uint8 VRAMBuffer=0,PPUGenLatch=0;
uint8 *vnapage[4];
uint8 PPUNTARAM=0;  
uint8 PPUCHRRAM=0;  

//Color deemphasis emulation.  Joy...
static uint8 deemp=0;
static int deempcnt[8];

void (*GameHBIRQHook)(void), (*GameHBIRQHook2)(void);
void (*PPU_hook)(uint32 A);

uint8 vtoggle=0;
uint8 XOffset=0;

uint32 TempAddr=0,RefreshAddr=0;

static int maxsprites=8;  

//scanline is equal to the current visible scanline we're on.
int scanline;
static uint32 scanlines_per_frame;

uint8 PPU[4];
uint8 PPUSPL;
uint8 NTARAM[0x800],PALRAM[0x20],SPRAM[0x100],SPRBUF[0x100];




#define MMC5SPRVRAMADR(V)      &MMC5SPRVPage[(V)>>10][(V)]
#define VRAMADR(V)      &VPage[(V)>>10][(V)]

//mbg 8/6/08 - fix a bug relating to
//"When in 8x8 sprite mode, only one set is used for both BG and sprites."
//in mmc5 docs
uint8 * MMC5BGVRAMADR(uint32 V) {
	if(!Sprite16) {
		extern uint8 mmc5ABMode;                /* A=0, B=1 */
		if(mmc5ABMode==0)
			return MMC5SPRVRAMADR(V);
		else 
			return &MMC5BGVPage[(V)>>10][(V)];
	} else return &MMC5BGVPage[(V)>>10][(V)];
}

//this duplicates logic which is embedded in the ppu rendering code
//which figures out where to get CHR data from depending on various hack modes
//mostly involving mmc5.
//this might be incomplete.
uint8* FCEUPPU_GetCHR(uint32 vadr, uint32 refreshaddr) {
	if(MMC5Hack) {
		if(MMC5HackCHRMode==1) {
			uint8 *C = MMC5HackVROMPTR;
			C += (((MMC5HackExNTARAMPtr[refreshaddr & 0x3ff]) & 0x3f & MMC5HackVROMMask) << 12) + (vadr & 0xfff);
			return C;
		} else {
			return MMC5BGVRAMADR(vadr);
		}
	}
	else return VRAMADR(vadr);
}

//likewise for ATTR
int FCEUPPU_GetAttr(int ntnum, int xt, int yt) {
	int attraddr = 0x3C0+((yt>>2)<<3)+(xt>>2);
	int temp = (((yt&2)<<1)+(xt&2));
	int refreshaddr = xt+yt*32;
	if(MMC5Hack && MMC5HackCHRMode==1)
		return (MMC5HackExNTARAMPtr[refreshaddr & 0x3ff] & 0xC0)>>6;
	else
		return (vnapage[ntnum][attraddr] & (3<<temp)) >> temp;
}

//new ppu-----
void FFCEUX_PPUWrite_Default(uint32 A, uint8 V) {
	uint32 tmp = A;

	if(tmp>=0x3F00)
		{
			// hmmm....
			if(!(tmp&0xf))
				PALRAM[0x00]=PALRAM[0x04]=PALRAM[0x08]=PALRAM[0x0C]=V&0x3F;
			else if(tmp&3) PALRAM[(tmp&0x1f)]=V&0x3f;
		}
		else if(tmp<0x2000)
		{
			if(PPUCHRRAM&(1<<(tmp>>10)))
				VPage[tmp>>10][tmp]=V;
		}   
		else
		{
			if(PPUNTARAM&(1<<((tmp&0xF00)>>10)))
				vnapage[((tmp&0xF00)>>10)][tmp&0x3FF]=V;
		}
}

uint8 FFCEUX_PPURead_Default(uint32 A) {
	uint32 tmp = A;

	if(tmp<0x2000)
	{
		return VPage[tmp>>10][tmp];
	}
	else
	{   
		return vnapage[(tmp>>10)&0x3][tmp&0x3FF];
	}
}


uint8 (*FFCEUX_PPURead)(uint32 A) = FFCEUX_PPURead_Default;
void (*FFCEUX_PPUWrite)(uint32 A, uint8 V) = FFCEUX_PPUWrite_Default;
//whether to use the new ppu
int newppu=0;
//---------------

static DECLFR(A2002)
{
	if(newppu)
	{
		//once we thought we clear latches here, but that caused midframe glitches.
		//i think we should only reset the state machine for 2005/2006
		//ppur.clear_latches();
	}
	
	uint8 ret;

	FCEUPPU_LineUpdate();
	ret = PPU_status;
	ret|=PPUGenLatch&0x1F;

#ifdef FCEUDEF_DEBUGGER
	if(!fceuindbg)
#endif
	{
		vtoggle=0;
		PPU_status&=0x7F;
		PPUGenLatch=ret;
	}

	return ret;
}

static DECLFR(A200x)  /* Not correct for $2004 reads. */
{
	FCEUPPU_LineUpdate();
	return PPUGenLatch;
}

/*
static DECLFR(A2004)
{
uint8 ret;

FCEUPPU_LineUpdate();
ret = SPRAM[PPU[3]];

if(PPUSPL>=8) 
{
if(PPU[3]>=8)
ret = SPRAM[PPU[3]];
}
else
{
//printf("$%02x:$%02x\n",PPUSPL,V);
ret = SPRAM[PPUSPL];
}
PPU[3]++;
PPUSPL++;
PPUGenLatch = ret;
printf("%d, %02x\n",scanline,ret);
return(ret);
}
*/
static DECLFR(A2007)
{
	uint8 ret;
	uint32 tmp=RefreshAddr&0x3FFF;

	if(newppu) {
		//mbg
		ret = VRAMBuffer;
		RefreshAddr = ppur.get_2007access();
		VRAMBuffer = FFCEUX_PPURead(RefreshAddr);
		ppur.increment2007(INC32!=0);
		RefreshAddr = ppur.get_2007access();
		return ret;
	} else {
		FCEUPPU_LineUpdate();

		ret=VRAMBuffer;

	#ifdef FCEUDEF_DEBUGGER
		if(!fceuindbg)
	#endif
		{
			if(PPU_hook) PPU_hook(tmp);
			PPUGenLatch=VRAMBuffer;
			if(tmp<0x2000)
			{
				VRAMBuffer=VPage[tmp>>10][tmp];
			}
			else
			{   
				VRAMBuffer=vnapage[(tmp>>10)&0x3][tmp&0x3FF];
			}
		}
	#ifdef FCEUDEF_DEBUGGER
		if(!fceuindbg)
	#endif
		{
			if(INC32) RefreshAddr+=32;
			else RefreshAddr++;
			if(PPU_hook) PPU_hook(RefreshAddr&0x3fff);
		}

		return ret;
	}
}

static DECLFW(B2000)
{
	//    FCEU_printf("%04x:%02x, (%d) %02x, %02x\n",A,V,scanline,PPU[0],PPU_status);

	FCEUPPU_LineUpdate();
	PPUGenLatch=V;
	if(!(PPU[0]&0x80) && (V&0x80) && (PPU_status&0x80))
	{
		//     FCEU_printf("Trigger NMI, %d, %d\n",timestamp,ppudead);
		TriggerNMI2();
	}
	PPU[0]=V;
	TempAddr&=0xF3FF;
	TempAddr|=(V&3)<<10;

	ppur._h = V&1;
	ppur._v = (V>>1)&1;
	ppur.s = (V>>4)&1;
}

static DECLFW(B2001)
{
	//printf("%04x:$%02x, %d\n",A,V,scanline);
	FCEUPPU_LineUpdate();
	PPUGenLatch=V;
	PPU[1]=V;
	if(V&0xE0)
		deemp=V>>5;
}

static DECLFW(B2002)
{
	PPUGenLatch=V;
}

static DECLFW(B2003)
{
	//printf("$%04x:$%02x, %d, %d\n",A,V,timestamp,scanline);
	PPUGenLatch=V;
	PPU[3]=V;
	PPUSPL=V&0x7;
}

static DECLFW(B2004)
{
	//printf("Wr: %04x:$%02x\n",A,V);

	PPUGenLatch=V;
	if(PPUSPL>=8) 
	{
		if(PPU[3]>=8)
			SPRAM[PPU[3]]=V;
	}
	else
	{   
		//printf("$%02x:$%02x\n",PPUSPL,V);
		SPRAM[PPUSPL]=V;
	}
	PPU[3]++;
	PPUSPL++;

}

static DECLFW(B2005)
{
	uint32 tmp=TempAddr;
	FCEUPPU_LineUpdate();
	PPUGenLatch=V;
	if(!vtoggle)
	{
		tmp&=0xFFE0;
		tmp|=V>>3;  
		XOffset=V&7;
		ppur._ht = V>>3;
		ppur.fh = V&7;
	}
	else
	{   
		tmp&=0x8C1F;
		tmp|=((V&~0x7)<<2);
		tmp|=(V&7)<<12;
		ppur._vt = V>>3;
		ppur._fv = V&7;
	}
	TempAddr=tmp;
	vtoggle^=1;  
}


static DECLFW(B2006)
{
	if(!newppu)
		FCEUPPU_LineUpdate();


	PPUGenLatch=V;
	if(!vtoggle)  
	{
		TempAddr&=0x00FF;
		TempAddr|=(V&0x3f)<<8;
		
		ppur._vt &= 0x07;
		ppur._vt |= (V&0x3)<<3;
		ppur._h = (V>>2)&1;
		ppur._v = (V>>3)&1;
		ppur._fv = (V>>4)&3;
	}
 	else
	{   
		TempAddr&=0xFF00;
		TempAddr|=V;

		RefreshAddr=TempAddr;
		if(PPU_hook)
			PPU_hook(RefreshAddr);
		//printf("%d, %04x\n",scanline,RefreshAddr);

		ppur._vt &= 0x18;
		ppur._vt |= (V>>5);
		ppur._ht = V&31;

		ppur.install_latches();

		if(RefreshAddr==0x18DE) {
			int zzz=9;
		}
	}

	if(ppur._fv == 1) {
		int zzz=9;
	}
	vtoggle^=1;
}

static DECLFW(B2007)
{
	uint32 tmp=RefreshAddr&0x3FFF;

	if(newppu) {
		RefreshAddr = ppur.get_2007access();
		FFCEUX_PPUWrite(RefreshAddr,V);
		if(RefreshAddr == 0x2679) {
			int zzz=9;
		}
		if(RefreshAddr == 0x3f13 ) {
			int zzz=9;
		}
		//printf("%04x ",RefreshAddr);
		ppur.increment2007(INC32!=0);
		RefreshAddr = ppur.get_2007access();
	} 
	else
	{
		//printf("%04x ",tmp);
		if(tmp==0x2679)
		{
			int zzz=9;
		}
				if(tmp == 0x3f13 ) {
			int zzz=9;
		}
		PPUGenLatch=V;
		if(tmp>=0x3F00)
		{
			// hmmm....
			if(!(tmp&0xf))
				PALRAM[0x00]=PALRAM[0x04]=PALRAM[0x08]=PALRAM[0x0C]=V&0x3F;
			else if(tmp&3) PALRAM[(tmp&0x1f)]=V&0x3f;
		}
		else if(tmp<0x2000)
		{
			if(PPUCHRRAM&(1<<(tmp>>10)))
				VPage[tmp>>10][tmp]=V;
		}   
		else
		{
			if(PPUNTARAM&(1<<((tmp&0xF00)>>10)))
				vnapage[((tmp&0xF00)>>10)][tmp&0x3FF]=V;
		}
		//      FCEU_printf("ppu (%04x) %04x:%04x %d, %d\n",X.PC,RefreshAddr,PPUGenLatch,scanline,timestamp);
		if(INC32) RefreshAddr+=32;
		else RefreshAddr++;
		if(PPU_hook) PPU_hook(RefreshAddr&0x3fff);
	}
}

static DECLFW(B4014)
{
	uint32 t=V<<8;
	int x;

	for(x=0;x<256;x++)
		X6502_DMW(0x2004,X6502_DMR(t+x));
}

#define PAL(c)  ((c)+cc)

#define GETLASTPIXEL    (PAL?((timestamp*48-linestartts)/15) : ((timestamp*48-linestartts)>>4) )

static uint8 *Pline,*Plinef;
static int firsttile;  
static int linestartts;
static int tofix=0;

static void ResetRL(uint8 *target)
{
	memset(target,0xFF,256);
	InputScanlineHook(0,0,0,0);
	Plinef=target;
	Pline=target; 
	firsttile=0;
	linestartts=timestamp*48+X.count;
	tofix=0;
	FCEUPPU_LineUpdate();
	tofix=1;  
}

static uint8 sprlinebuf[256+8];

void FCEUPPU_LineUpdate(void)
{
#ifdef FCEUDEF_DEBUGGER
	if(!fceuindbg)
#endif
		if(Pline)
		{
			int l=GETLASTPIXEL;
			RefreshLine(l);
		}
} 

static bool rendersprites=true, renderbg=true;

void FCEUI_SetRenderPlanes(bool sprites, bool bg)
{
	rendersprites = sprites;
	renderbg = bg;
}

void FCEUI_GetRenderPlanes(bool& sprites, bool& bg)
{
	sprites = rendersprites;
	bg = renderbg;
}

//mbg 6/21/08 - tileview is being ripped out since i dont know how long its been since it worked
//static int tileview=1;
//void FCEUI_ToggleTileView(void)
//{
//	tileview^=1;
//}


//mbg 6/21/08 - tileview is being ripped out since i dont know how long its been since it worked
//static void TileView(void)
//{
//	uint8 *P=XBuf+16*256;
//	int bgh;
//	int y;  
//	int X1; 
//	for(bgh=0;bgh<2;bgh++)
//		for(y=0;y<16*8;y++)   
//			for(P=XBuf+bgh*128+(16+y)*256,X1=16;X1;X1--,P+=8)
//			{
//				uint8 *C;
//				register uint8 cc;
//				uint32 vadr;
//
//				vadr=((((16-X1)|((y>>3)<<4))<<4)|(y&7))+bgh*0x1000;
//				//C= ROM+vadr+turt*8192;
//				C = VRAMADR(vadr);
//				//if((vadr+turt*8192)>=524288)
//				//printf("%d ",vadr+turt*8192);
//				cc=0;
//				//#include "pputile.inc"
//			}
//} 

static void CheckSpriteHit(int p);

static void EndRL(void)
{
	RefreshLine(272);
	if(tofix)
		Fixit1();
	CheckSpriteHit(272);
	Pline=0;
}

static int32 sphitx;
static uint8 sphitdata;

static void CheckSpriteHit(int p)
{
	int l=p-16;
	int x;

	if(sphitx==0x100) return;

	for(x=sphitx;x<(sphitx+8) && x<l;x++)
	{
		if((sphitdata&(0x80>>(x-sphitx))) && !(Plinef[x]&64))
		{
			PPU_status|=0x40;
			//printf("Ha:  %d, %d, Hita: %d, %d, %d, %d, %d\n",p,p&~7,scanline,GETLASTPIXEL-16,&Plinef[x],Pline,Pline-Plinef);
			//printf("%d\n",GETLASTPIXEL-16);
			//if(Plinef[x] == 0xFF)
			//printf("PL: %d, %02x\n",scanline, Plinef[x]);
			sphitx=0x100;
			break;
		}
	}
}   

//spork the world.  Any sprites on this line? Then this will be set to 1.  
//Needed for zapper emulation and *gasp* sprite emulation.
static int spork=0;     
						
// lasttile is really "second to last tile."
static void RefreshLine(int lastpixel)
{
	static uint32 pshift[2];
	static uint32 atlatch;
	uint32 smorkus=RefreshAddr;

#define RefreshAddr smorkus
	uint32 vofs;
	int X1;

	register uint8 *P=Pline;
	int lasttile=lastpixel>>3;
	int numtiles;
	static int norecurse=0; /* Yeah, recursion would be bad.
							PPU_hook() functions can call
							mirroring/chr bank switching functions,
							which call FCEUPPU_LineUpdate, which call this
							function. */
	if(norecurse) return;

	if(sphitx != 0x100 && !(PPU_status&0x40))
	{
		if((sphitx < (lastpixel-16)) && !(sphitx < ((lasttile - 2)*8)))
		{
			//printf("OK: %d\n",scanline);
			lasttile++;
		}

	}

	if(lasttile>34) lasttile=34;
	numtiles=lasttile-firsttile;

	if(numtiles<=0) return;

	P=Pline;

	vofs=0;

	vofs=((PPU[0]&0x10)<<8) | ((RefreshAddr>>12)&7);

	if(!ScreenON && !SpriteON)
	{
		uint32 tem;
		tem=Pal[0]|(Pal[0]<<8)|(Pal[0]<<16)|(Pal[0]<<24);
		tem|=0x40404040;
		FCEU_dwmemset(Pline,tem,numtiles*8);
		P+=numtiles*8;
		Pline=P;

		firsttile=lasttile;

#define TOFIXNUM (272-0x4)
		if(lastpixel>=TOFIXNUM && tofix)
		{
			Fixit1();
			tofix=0;
		}

		if((lastpixel-16)>=0) 
		{
			InputScanlineHook(Plinef,spork?sprlinebuf:0,linestartts,lasttile*8-16);
		}
		return;
	}

	//Priority bits, needed for sprite emulation.
	Pal[0]|=64; 
	Pal[4]|=64;
	Pal[8]|=64;
	Pal[0xC]|=64;

	//This high-level graphics MMC5 emulation code was written for MMC5 carts in "CL" mode.  
	//It's probably not totally correct for carts in "SL" mode.

#define PPUT_MMC5
	if(MMC5Hack && geniestage!=1)
	{
		if(MMC5HackCHRMode==0 && (MMC5HackSPMode&0x80))
		{
			int tochange=MMC5HackSPMode&0x1F;
			tochange-=firsttile;
			for(X1=firsttile;X1<lasttile;X1++)
			{
				if((tochange<=0 && MMC5HackSPMode&0x40) || (tochange>0 && !(MMC5HackSPMode&0x40)))
				{
#define PPUT_MMC5SP
#include "pputile.inc"
#undef PPUT_MMC5SP
				}
				else
				{
#include "pputile.inc"	    
				}
				tochange--;
			}
		}
		else if(MMC5HackCHRMode==1 && (MMC5HackSPMode&0x80))
		{
			int tochange=MMC5HackSPMode&0x1F;
			tochange-=firsttile;

#define PPUT_MMC5SP
#define PPUT_MMC5CHR1
			for(X1=firsttile;X1<lasttile;X1++)
			{
#include "pputile.inc"
			}
#undef PPUT_MMC5CHR1
#undef PPUT_MMC5SP
		}
		else if(MMC5HackCHRMode==1)
		{
#define PPUT_MMC5CHR1
			for(X1=firsttile;X1<lasttile;X1++)
			{
#include "pputile.inc"
			}
#undef PPUT_MMC5CHR1
		}
		else
		{
			for(X1=firsttile;X1<lasttile;X1++)
			{
#include "pputile.inc"
			}
		}
	}
#undef PPUT_MMC5
	else if(PPU_hook)
	{
		norecurse=1;
#define PPUT_HOOK
		for(X1=firsttile;X1<lasttile;X1++)
		{
#include "pputile.inc"
		}
#undef PPUT_HOOK
		norecurse=0;
	}
	else
	{
		for(X1=firsttile;X1<lasttile;X1++)
		{
#include "pputile.inc"
		}
	}

#undef vofs
#undef RefreshAddr

	//Reverse changes made before.
	Pal[0]&=63; 
	Pal[4]&=63;
	Pal[8]&=63;
	Pal[0xC]&=63;

	RefreshAddr=smorkus;
	if(firsttile<=2 && 2<lasttile && !(PPU[1]&2)) 
	{
		uint32 tem;
		tem=Pal[0]|(Pal[0]<<8)|(Pal[0]<<16)|(Pal[0]<<24);  
		tem|=0x40404040;
		*(uint32 *)Plinef=*(uint32 *)(Plinef+4)=tem;
	}

	if(!ScreenON)
	{
		uint32 tem;
		int tstart,tcount;
		tem=Pal[0]|(Pal[0]<<8)|(Pal[0]<<16)|(Pal[0]<<24);
		tem|=0x40404040;

		tcount=lasttile-firsttile;
		tstart=firsttile-2;
		if(tstart<0)
		{
			tcount+=tstart;
			tstart=0;
		}
		if(tcount>0)
			FCEU_dwmemset(Plinef+tstart*8,tem,tcount*8);
	}

	if(lastpixel>=TOFIXNUM && tofix)
	{
		//puts("Fixed");
		Fixit1();
		tofix=0;
	}

	//CheckSpriteHit(lasttile*8); //lasttile*8); //lastpixel);

	//This only works right because of a hack earlier in this function.
	CheckSpriteHit(lastpixel);  
	
	if((lastpixel-16)>=0)
	{
		InputScanlineHook(Plinef,spork?sprlinebuf:0,linestartts,lasttile*8-16);
	}
	Pline=P;
	firsttile=lasttile;
}

static INLINE void Fixit2(void)
{
	if(ScreenON || SpriteON)
	{
		uint32 rad=RefreshAddr;
		rad&=0xFBE0;
		rad|=TempAddr&0x041f;
		RefreshAddr=rad;
		//PPU_hook(RefreshAddr);
		//PPU_hook(RefreshAddr,-1);
	}
}

static void Fixit1(void)
{
	if(ScreenON || SpriteON)
	{
		uint32 rad=RefreshAddr;

		if((rad&0x7000)==0x7000)
		{
			rad^=0x7000;
			if((rad&0x3E0)==0x3A0)
			{
				rad^=0x3A0;
				rad^=0x800;
			}
			else
			{
				if((rad&0x3E0)==0x3e0)
					rad^=0x3e0;
				else rad+=0x20;
			}
		}
		else
			rad+=0x1000;
		RefreshAddr=rad;
		//PPU_hook(RefreshAddr); //,-1);
	}
}

void MMC5_hb(int);     //Ugh ugh ugh.
static void DoLine(void)
{
	int x;
	uint8 *target=XBuf+(scanline<<8);

	if(MMC5Hack && (ScreenON || SpriteON)) MMC5_hb(scanline);

	X6502_Run(256);
	EndRL();

	if(!renderbg)  // User asked to not display background data.
	{
		uint32 tem;
		uint8 col;
		if(gNoBGFillColor == 0xFF)
			col = Pal[0];
		else col = gNoBGFillColor;
		tem=col|(col<<8)|(col<<16)|(col<<24);
		tem|=0x40404040;
		FCEU_dwmemset(target,tem,256);
	}

	if(SpriteON)
		CopySprites(target);

	if(ScreenON || SpriteON)  // Yes, very el-cheapo.
	{
		if(PPU[1]&0x01)
		{
			for(x=63;x>=0;x--)
				*(uint32 *)&target[x<<2]=(*(uint32*)&target[x<<2])&0x30303030;
		}
	}
	if((PPU[1]>>5)==0x7)
	{
		for(x=63;x>=0;x--)
			*(uint32 *)&target[x<<2]=((*(uint32*)&target[x<<2])&0x3f3f3f3f)|0xc0c0c0c0;
	}
	else if(PPU[1]&0xE0)
		for(x=63;x>=0;x--)
			*(uint32 *)&target[x<<2]=(*(uint32*)&target[x<<2])|0x40404040;
	else
		for(x=63;x>=0;x--)
			*(uint32 *)&target[x<<2]=((*(uint32*)&target[x<<2])&0x3f3f3f3f)|0x80808080;

	sphitx=0x100;

	if(ScreenON || SpriteON)
		FetchSpriteData();

	if(GameHBIRQHook && (ScreenON || SpriteON) && ((PPU[0]&0x38)!=0x18))
	{
		X6502_Run(6);
		Fixit2();
		X6502_Run(4);
		GameHBIRQHook();
		X6502_Run(85-16-10);
	}
	else
	{
		X6502_Run(6);  // Tried 65, caused problems with Slalom(maybe others)
		Fixit2();
		X6502_Run(85-6-16);

		// A semi-hack for Star Trek: 25th Anniversary
		if(GameHBIRQHook && (ScreenON || SpriteON) && ((PPU[0]&0x38)!=0x18))
			GameHBIRQHook();
	}

	DEBUG(FCEUD_UpdateNTView(scanline,0));

	if(SpriteON)
		RefreshSprites();
	if(GameHBIRQHook2 && (ScreenON || SpriteON))
		GameHBIRQHook2();
	scanline++;
	if(scanline<240)
	{
		ResetRL(XBuf+(scanline<<8));
	}
	X6502_Run(16);
}

#define V_FLIP  0x80
#define H_FLIP  0x40
#define SP_BACK 0x20

typedef struct {
	uint8 y,no,atr,x;
} SPR;

typedef struct {
	uint8 ca[2],atr,x;
} SPRB;

void FCEUI_DisableSpriteLimitation(int a)
{
	maxsprites=a?64:8;
}

static uint8 numsprites,SpriteBlurp;
static void FetchSpriteData(void)
{
	uint8 ns,sb;
	SPR *spr;
	uint8 H;
	int n;
	int vofs;
	uint8 P0=PPU[0];

	spr=(SPR *)SPRAM;
	H=8;

	ns=sb=0;

	vofs=(unsigned int)(P0&0x8&(((P0&0x20)^0x20)>>2))<<9;
	H+=(P0&0x20)>>2;

	if(!PPU_hook)
		for(n=63;n>=0;n--,spr++)
		{
			if((unsigned int)(scanline-spr->y)>=H) continue;
			//printf("%d, %u\n",scanline,(unsigned int)(scanline-spr->y));
			if(ns<maxsprites)
			{
				if(n==63) sb=1;

				{
					SPRB dst;
					uint8 *C;
					int t;
					unsigned int vadr;

					t = (int)scanline-(spr->y);

					if(Sprite16)
						vadr = ((spr->no&1)<<12) + ((spr->no&0xFE)<<4);
					else
						vadr = (spr->no<<4)+vofs;

					if(spr->atr&V_FLIP)
					{
						vadr+=7;
						vadr-=t;
						vadr+=(P0&0x20)>>1;
						vadr-=t&8;
					}
					else
					{
						vadr+=t;
						vadr+=t&8;
					}

					/* Fix this geniestage hack */
					if(MMC5Hack && geniestage!=1) C = MMC5SPRVRAMADR(vadr);
					else C = VRAMADR(vadr);


					dst.ca[0]=C[0];
					dst.ca[1]=C[8];
					dst.x=spr->x;
					dst.atr=spr->atr;

					*(uint32 *)&SPRBUF[ns<<2]=*(uint32 *)&dst;
				}

				ns++;
			}
			else
			{
				PPU_status|=0x20;
				break;
			}
		}
	else
		for(n=63;n>=0;n--,spr++)
		{
			if((unsigned int)(scanline-spr->y)>=H) continue;

			if(ns<maxsprites)
			{
				if(n==63) sb=1;

				{
					SPRB dst;
					uint8 *C;
					int t;
					unsigned int vadr;

					t = (int)scanline-(spr->y);

					if(Sprite16)
						vadr = ((spr->no&1)<<12) + ((spr->no&0xFE)<<4);
					else
						vadr = (spr->no<<4)+vofs;

					if(spr->atr&V_FLIP)
					{
						vadr+=7;
						vadr-=t;
						vadr+=(P0&0x20)>>1;
						vadr-=t&8;
					}
					else
					{
						vadr+=t;
						vadr+=t&8;
					}

					if(MMC5Hack) C = MMC5SPRVRAMADR(vadr);
					else C = VRAMADR(vadr);
					dst.ca[0]=C[0];
					if(ns<8)
					{
						PPU_hook(0x2000);
						PPU_hook(vadr);
					}
					dst.ca[1]=C[8];
					dst.x=spr->x;
					dst.atr=spr->atr;


					*(uint32 *)&SPRBUF[ns<<2]=*(uint32 *)&dst;
				}

				ns++;
			}
			else
			{
				PPU_status|=0x20;
				break;
			}
		}
		//if(ns>=7)
		//printf("%d %d\n",scanline,ns);
		
		//Handle case when >8 sprites per scanline option is enabled. 
		if(ns>8) PPU_status|=0x20;  
		else if(PPU_hook)
		{
			for(n=0;n<(8-ns);n++)
			{
				PPU_hook(0x2000);
				PPU_hook(vofs);
			}
		}
		numsprites=ns;
		SpriteBlurp=sb;
}

static void RefreshSprites(void)
{
	int n;
	SPRB *spr;

	spork=0;
	if(!numsprites) return;

	FCEU_dwmemset(sprlinebuf,0x80808080,256);
	numsprites--;
	spr = (SPRB*)SPRBUF+numsprites;

	for(n=numsprites;n>=0;n--,spr--)
	{
		uint32 pixdata;
		uint8 J,atr;

		int x=spr->x;
		uint8 *C;
		uint8 *VB;

		pixdata=ppulut1[spr->ca[0]]|ppulut2[spr->ca[1]];
		J=spr->ca[0]|spr->ca[1];
		atr=spr->atr;

		if(J)
		{
			if(n==0 && SpriteBlurp && !(PPU_status&0x40))
			{
				sphitx=x;
				sphitdata=J;
				if(atr&H_FLIP)
					sphitdata=    ((J<<7)&0x80) |
					((J<<5)&0x40) |
					((J<<3)&0x20) |
					((J<<1)&0x10) |
					((J>>1)&0x08) |
					((J>>3)&0x04) |
					((J>>5)&0x02) |
					((J>>7)&0x01);                                          
			}

			C = sprlinebuf+x;
			VB = (PALRAM+0x10)+((atr&3)<<2);

			if(atr&SP_BACK) 
			{
				if(atr&H_FLIP)
				{
					if(J&0x80) C[7]=VB[pixdata&3]|0x40;
					pixdata>>=4;
					if(J&0x40) C[6]=VB[pixdata&3]|0x40;
					pixdata>>=4;
					if(J&0x20) C[5]=VB[pixdata&3]|0x40;
					pixdata>>=4;
					if(J&0x10) C[4]=VB[pixdata&3]|0x40;
					pixdata>>=4;
					if(J&0x08) C[3]=VB[pixdata&3]|0x40;
					pixdata>>=4;
					if(J&0x04) C[2]=VB[pixdata&3]|0x40;
					pixdata>>=4;
					if(J&0x02) C[1]=VB[pixdata&3]|0x40;
					pixdata>>=4;
					if(J&0x01) C[0]=VB[pixdata]|0x40;
				} else  {
					if(J&0x80) C[0]=VB[pixdata&3]|0x40;   
					pixdata>>=4;
					if(J&0x40) C[1]=VB[pixdata&3]|0x40;   
					pixdata>>=4;
					if(J&0x20) C[2]=VB[pixdata&3]|0x40;
					pixdata>>=4;
					if(J&0x10) C[3]=VB[pixdata&3]|0x40;
					pixdata>>=4;
					if(J&0x08) C[4]=VB[pixdata&3]|0x40;
					pixdata>>=4;
					if(J&0x04) C[5]=VB[pixdata&3]|0x40;
					pixdata>>=4;
					if(J&0x02) C[6]=VB[pixdata&3]|0x40;
					pixdata>>=4;
					if(J&0x01) C[7]=VB[pixdata]|0x40;
				}
			} else {
				if(atr&H_FLIP)
				{
					if(J&0x80) C[7]=VB[pixdata&3];   
					pixdata>>=4;
					if(J&0x40) C[6]=VB[pixdata&3];   
					pixdata>>=4;
					if(J&0x20) C[5]=VB[pixdata&3];
					pixdata>>=4;
					if(J&0x10) C[4]=VB[pixdata&3];
					pixdata>>=4;
					if(J&0x08) C[3]=VB[pixdata&3];
					pixdata>>=4;
					if(J&0x04) C[2]=VB[pixdata&3];
					pixdata>>=4;
					if(J&0x02) C[1]=VB[pixdata&3];
					pixdata>>=4;
					if(J&0x01) C[0]=VB[pixdata];
				}else{                 
					if(J&0x80) C[0]=VB[pixdata&3];   
					pixdata>>=4;
					if(J&0x40) C[1]=VB[pixdata&3];   
					pixdata>>=4;
					if(J&0x20) C[2]=VB[pixdata&3];
					pixdata>>=4;
					if(J&0x10) C[3]=VB[pixdata&3];
					pixdata>>=4;
					if(J&0x08) C[4]=VB[pixdata&3];
					pixdata>>=4;
					if(J&0x04) C[5]=VB[pixdata&3];
					pixdata>>=4;
					if(J&0x02) C[6]=VB[pixdata&3];
					pixdata>>=4;
					if(J&0x01) C[7]=VB[pixdata];
				}
			}
		}
	}
	SpriteBlurp=0;
	spork=1;
}

static void CopySprites(uint8 *target)
{
	uint8 n=((PPU[1]&4)^4)<<1;
	uint8 *P=target;

	if(!spork) return;
	spork=0;

	if(!rendersprites) return;  //User asked to not display sprites.

loopskie:
	{
		uint32 t=*(uint32 *)(sprlinebuf+n);

		if(t!=0x80808080)
		{
#ifdef LSB_FIRST
			if(!(t&0x80))
			{
				if(!(t&0x40) || (P[n]&0x40))       // Normal sprite || behind bg sprite
					P[n]=sprlinebuf[n];
			}

			if(!(t&0x8000))
			{
				if(!(t&0x4000) || (P[n+1]&0x40))       // Normal sprite || behind bg sprite
					P[n+1]=(sprlinebuf+1)[n];
			}

			if(!(t&0x800000))
			{
				if(!(t&0x400000) || (P[n+2]&0x40))       // Normal sprite || behind bg sprite
					P[n+2]=(sprlinebuf+2)[n];
			}

			if(!(t&0x80000000))
			{
				if(!(t&0x40000000) || (P[n+3]&0x40))       // Normal sprite || behind bg sprite
					P[n+3]=(sprlinebuf+3)[n];
			}
#else
			/* TODO:  Simplify */
			if(!(t&0x80000000))
			{
				if(!(t&0x40000000))       // Normal sprite
					P[n]=sprlinebuf[n];
				else if(P[n]&64)  // behind bg sprite
					P[n]=sprlinebuf[n];
			}

			if(!(t&0x800000))
			{
				if(!(t&0x400000))       // Normal sprite
					P[n+1]=(sprlinebuf+1)[n];
				else if(P[n+1]&64)  // behind bg sprite
					P[n+1]=(sprlinebuf+1)[n];
			}

			if(!(t&0x8000))
			{
				if(!(t&0x4000))       // Normal sprite
					P[n+2]=(sprlinebuf+2)[n];
				else if(P[n+2]&64)  // behind bg sprite
					P[n+2]=(sprlinebuf+2)[n];
			}

			if(!(t&0x80))
			{
				if(!(t&0x40))       // Normal sprite
					P[n+3]=(sprlinebuf+3)[n];
				else if(P[n+3]&64)  // behind bg sprite
					P[n+3]=(sprlinebuf+3)[n];
			}
#endif
		}
	}
	n+=4;
	if(n) goto loopskie;
}

void FCEUPPU_SetVideoSystem(int w)
{
	if(w)
	{
		scanlines_per_frame=312;
		FSettings.FirstSLine=FSettings.UsrFirstSLine[1];
		FSettings.LastSLine=FSettings.UsrLastSLine[1];
	}
	else
	{
		scanlines_per_frame=262;
		FSettings.FirstSLine=FSettings.UsrFirstSLine[0];
		FSettings.LastSLine=FSettings.UsrLastSLine[0];
	}
}

//Initializes the PPU
void FCEUPPU_Init(void)
{
	makeppulut();
}

void FCEUPPU_Reset(void)
{
	VRAMBuffer=PPU[0]=PPU[1]=PPU_status=PPU[3]=0;   
	PPUSPL=0;
	PPUGenLatch=0;
	RefreshAddr=TempAddr=0;
	vtoggle = 0;
	ppudead = 2;
	kook = 0;

	//	XOffset=0;
}

void FCEUPPU_Power(void)
{
	int x;

	memset(NTARAM,0x00,0x800);
	memset(PALRAM,0x00,0x20); 
	memset(SPRAM,0x00,0x100); 
	FCEUPPU_Reset();

	for(x=0x2000;x<0x4000;x+=8)
	{
		ARead[x]=A200x;
		BWrite[x]=B2000;
		ARead[x+1]=A200x;
		BWrite[x+1]=B2001;
		ARead[x+2]=A2002;
		BWrite[x+2]=B2002;
		ARead[x+3]=A200x;
		BWrite[x+3]=B2003;
		ARead[x+4]=A200x; //A2004;
		BWrite[x+4]=B2004;
		ARead[x+5]=A200x;
		BWrite[x+5]=B2005;
		ARead[x+6]=A200x;
		BWrite[x+6]=B2006;
		ARead[x+7]=A2007;
		BWrite[x+7]=B2007;
	}
	BWrite[0x4014]=B4014;
}

int FCEUPPU_Loop(int skip)
{
	if(newppu) {
		int FCEUX_PPU_Loop(int skip);
		return FCEUX_PPU_Loop(skip);
	}

	//Needed for Knight Rider, possibly others.
	if(ppudead)
	{
		memset(XBuf, 0x80, 256*240);
		X6502_Run(scanlines_per_frame*(256+85));
		ppudead--;
	}
	else
	{
		X6502_Run(256+85);
		PPU_status |= 0x80;
		
		//Not sure if this is correct.  According to Matt Conte and my own tests, it is.
		//Timing is probably off, though.  
		//NOTE:  Not having this here breaks a Super Donkey Kong game. 
		PPU[3]=PPUSPL=0;       
							   
		//I need to figure out the true nature and length of this delay.
		X6502_Run(12);
		if(GameInfo->type==GIT_NSF)
			DoNSFFrame();
		else
		{
			if(VBlankON)
				TriggerNMI();
		}
		X6502_Run((scanlines_per_frame-242)*(256+85)-12); //-12); 
		PPU_status&=0x1f;
		X6502_Run(256);

		{
			int x;

			if(ScreenON || SpriteON)
			{
				if(GameHBIRQHook && ((PPU[0]&0x38)!=0x18))
					GameHBIRQHook();
				if(PPU_hook)
					for(x=0;x<42;x++) {PPU_hook(0x2000); PPU_hook(0);}
					if(GameHBIRQHook2)
						GameHBIRQHook2();
			}
			X6502_Run(85-16);
			if(ScreenON || SpriteON)
			{  
				RefreshAddr=TempAddr;  
				if(PPU_hook) PPU_hook(RefreshAddr&0x3fff);  
			}

			//Clean this stuff up later.
			spork=numsprites=0;
			ResetRL(XBuf);

			X6502_Run(16-kook);
			kook ^= 1;
		}
		if(GameInfo->type==GIT_NSF)
			X6502_Run((256+85)*240);
#ifdef FRAMESKIP
		else if(skip)
		{
			int y;

			y=SPRAM[0];
			y++;

			PPU_status|=0x20;       // Fixes "Bee 52".  Does it break anything?
			if(GameHBIRQHook)
			{
				X6502_Run(256);
				for(scanline=0;scanline<240;scanline++)
				{
					if(ScreenON || SpriteON)
						GameHBIRQHook();
					if(scanline==y && SpriteON) PPU_status|=0x40;
					X6502_Run((scanline==239)?85:(256+85));
				}    
			}
			else if(y<240)
			{
				X6502_Run((256+85)*y);
				if(SpriteON) PPU_status|=0x40; // Quick and very dirty hack.
				X6502_Run((256+85)*(240-y));
			}
			else
				X6502_Run((256+85)*240);
		}
#endif
		else
		{
			int x,max,maxref;

			deemp=PPU[1]>>5;
			for(scanline=0;scanline<240;)       //scanline is incremented in  DoLine.  Evil. :/
			{
				deempcnt[deemp]++;
				DEBUG(FCEUD_UpdatePPUView(scanline, 1));
				DoLine();
			}
			if(MMC5Hack && (ScreenON || SpriteON)) MMC5_hb(scanline);
			for(x=1,max=0,maxref=0;x<7;x++)
			{
				if(deempcnt[x]>max)
				{
					max=deempcnt[x];
					maxref=x;
				}
				deempcnt[x]=0;
			}
			//FCEU_DispMessage("%2x:%2x:%2x:%2x:%2x:%2x:%2x:%2x %d",deempcnt[0],deempcnt[1],deempcnt[2],deempcnt[3],deempcnt[4],deempcnt[5],deempcnt[6],deempcnt[7],maxref);
			//memset(deempcnt,0,sizeof(deempcnt));
			SetNESDeemph(maxref,0);
		}
	} //else... to if(ppudead)

#ifdef FRAMESKIP   
	if(skip)
	{
		FCEU_PutImageDummy();
		return(0);
	}
	else
#endif
	{
		//mbg 6/21/08 - tileview is being ripped out since i dont know how long its been since it worked
		//if(tileview) TileView();
		FCEU_PutImage();     
		return(1);
	}   
}

int (*PPU_MASTER)(int skip) = FCEUPPU_Loop;

static uint16 TempAddrT,RefreshAddrT;

void FCEUPPU_LoadState(int version)
{
	TempAddr=TempAddrT;
	RefreshAddr=RefreshAddrT;
}

SFORMAT FCEUPPU_STATEINFO[]={
	{ NTARAM, 0x800, "NTAR"},
	{ PALRAM, 0x20, "PRAM"},
	{ SPRAM, 0x100, "SPRA"},
	{ PPU, 0x4, "PPUR"},
	{ &kook, 1, "KOOK"},
	{ &ppudead, 1, "DEAD"},
	{ &PPUSPL, 1, "PSPL"},
	{ &XOffset, 1, "XOFF"},
	{ &vtoggle, 1, "VTOG"},
	{ &RefreshAddrT, 2|FCEUSTATE_RLSB, "RADD"},
	{ &TempAddrT, 2|FCEUSTATE_RLSB, "TADD"},
	{ &VRAMBuffer, 1, "VBUF"},
	{ &PPUGenLatch, 1, "PGEN"},
	{ 0 }
};

void FCEUPPU_SaveState(void)
{
	TempAddrT=TempAddr;   
	RefreshAddrT=RefreshAddr;
}


//---------------------
int pputime=0;
int totpputime=0;
const int kLineTime=1364;
const int kFetchTime=8;
int idleSynch = 0;
void runcpu() {
	//cpu runs 1/12 as fast as ppu.
	if(pputime<12) return;
	int cputodo = pputime/12;
	X6502_Run(cputodo*3); //why *3 i dunno thats how the cpu core is setup
	pputime -= cputodo*12;
}
void runppu(int x) {
	pputime+=x;
	totpputime+=x;
	runcpu();
}

struct BGData {
		struct Record {
			uint8 nt, at, pt[2];

			void Read() {
				RefreshAddr = ppur.get_ntread();
				nt = FFCEUX_PPURead(RefreshAddr);
				runppu(kFetchTime);

				RefreshAddr = ppur.get_atread();
				at = FFCEUX_PPURead(RefreshAddr);
				runppu(kFetchTime);

				//modify at to get appropriate palette shift
				if(ppur.vt&2) at >>= 4;
				if(ppur.ht&2) at >>= 2;
				at &= 0x03;
				at <<= 2;

				ppur.par = nt;
				RefreshAddr = ppur.get_ptread();
				pt[0] = FFCEUX_PPURead(RefreshAddr);
				runppu(kFetchTime);
				RefreshAddr |= 8;
				pt[1] = FFCEUX_PPURead(RefreshAddr);
				runppu(kFetchTime);

				if(PPUON)
					ppur.increment_hsc();
			}
		};

		Record main[34]; //one at the end is junk, it can never be rendered
	} bgdata;


int framectr=0;
int FCEUX_PPU_Loop(int skip) {
	//262 scanlines

	if(ppudead)
	{
		memset(XBuf, 0x80, 256*240);
		runppu(262*kLineTime);
		ppudead--;
		goto finish;
	}

	{
		//vblank
		PPU_status |= 0x80;
		if(VBlankON) TriggerNMI();

		//TRICKY:
		//even though the timing doc says that every scanline is 1364 except for the first dummy scanline,
		//i need these to be 1360 in order to get marble madness and pirates! to work.
		runppu(20*1360);

		//no longer in vblank
		PPU_status &= ~0x80;
		//fceu cleared the spritehit and spriteoverflow flags at the same time.. it is reasonable here. or, it is resaonable at vblank interrupt time
		PPU_status &= ~0x40;
		PPU_status &= ~0x20;

		//----------
		//this early out caused metroid to fail to boot. I am leaving it here as a reminder of what not to do
		//if(!PPUON) {
		//	runppu(kLineTime*242);
		//	goto finish;
		//}
		//----------

		//There are 2 conditions that update all 5 PPU scroll counters with the 
		//contents of the latches adjacent to them. The first is after a write to 
		//2006/2. The second, is at the beginning of scanline 20, when the PPU starts 
		//rendering data for the first time in a frame (this update won't happen if 
		//all rendering is disabled via 2001.3 and 2001.4).

		if(PPUON)
			ppur.install_latches();

		uint8 oams[2][8][7];
		int oamcounts[2]={0,0};
		int oamslot=0;
		int oamcount;

		//capture the initial xscroll
		int xscroll = ppur.fh;

		//render 241 scanlines (including 1 dummy at beginning)
		for(int sl=0;sl<241;sl++) {
			int yp = sl-1;

			if(sl != 0) {
				DEBUG(FCEUD_UpdatePPUView(scanline=yp,1));
				DEBUG(FCEUD_UpdateNTView(scanline=yp,1));
			}

			//twiddle the oam buffers
			int scanslot = oamslot^1;
			int renderslot = oamslot;
			oamslot ^= 1;

			oamcount = oamcounts[renderslot];
			
			//the main scanline rendering loop:
			//32 times, we will fetch a tile and then render 8 pixels.
			//two of those tiles were read in the last scanline.
			for(int xt=0;xt<32;xt++) {
				bgdata.main[xt+2].Read();

				//ok, we're also going to draw here.
				//unless we're on the first dummy scanline
				if(sl != 0) {
					int xstart = xt<<3;
					oamcount = oamcounts[renderslot];
					uint8 *target=XBuf+(yp<<8)+xstart;
					uint8 *ptr = target;
					int rasterpos = xstart;

					//check all the conditions that can cause things to render in these 8px
					bool renderspritenow = SpriteON && rendersprites && (xt>0 || SpriteLeft8);
					bool renderbgnow = ScreenON && renderbg && (xt>0 || BGLeft8);

					for(int xp=0;xp<8;xp++,rasterpos++) {

						//bg pos is different from raster pos due to its offsetability.
						//so adjust for that here
						int bgpos = rasterpos + xscroll;
						int bgpx = bgpos&7;
						int bgtile = bgpos>>3;
						
						uint8 pixel=0, pixelcolor;

						//generate the BG data
						if(renderbgnow)
						{
							uint8* pt = bgdata.main[bgtile].pt;
							pixel = ((pt[0]>>(7-bgpx))&1) | (((pt[1]>>(7-bgpx))&1)<<1) | bgdata.main[bgtile].at;
						}
						pixelcolor = PALRAM[pixel];

						//look for a sprite to be drawn
						if(renderspritenow) {
							bool havepixel = false;
							for(int s=0;s<oamcount;s++) {
								uint8* oam = oams[renderslot][s];
								int x = oam[3]; 
								if(rasterpos>=x && rasterpos<x+8) {
									//build the pixel.
									//fetch the LSB of the patterns
									uint8 spixel = oam[4]&1;
									spixel |= (oam[5]&1)<<1;

									//shift down the patterns so the next pixel is in the LSB
									oam[4] >>= 1;
									oam[5] >>= 1;

									//bail out if we already have a pixel from a higher priority sprite
									if(havepixel) continue;

									//transparent pixel bailout
									if(spixel==0) continue;

									//spritehit:
									//1. is it sprite#0?
									//2. is the bg pixel nonzero?
									//then, it is spritehit.
									if(oam[6] == 0 && pixel != 0)
										PPU_status |= 0x40;

									havepixel = true;

									//priority handling
									if(oam[2]&0x20) {
										//behind background:
										if((pixel&3)!=0) continue;
									}

									//bring in the palette bits and palettize
									spixel |= (oam[2]&3)<<2;
									pixelcolor  = PALRAM[0x10+spixel];
								}
							}
						}

						//fceu rendering system requires that this be set 
						//(so that it knows there is a valid pixel there?)
						pixelcolor |= 0x80;
						*ptr++ = pixelcolor;
					}
				}
			}

			//look for sprites (was supposed to run concurrent with bg rendering)
			oamcounts[scanslot] = 0;
			oamcount=0;
			int spriteHeight = Sprite16?16:8;
			for(int i=0;i<64;i++) {
				uint8* spr = SPRAM+i*4;
				if(yp >= spr[0] && yp < spr[0]+spriteHeight) {
					//if we already have 8 sprites, then this new one causes an overflow,
					//set the flag and bail out.
					if(oamcount == 8) {
						PPU_status |= 0x20;
						break;
					}

					//just copy some bytes into the internal sprite buffer
					for(int j=0;j<4;j++)
						oams[scanslot][oamcount][j] = spr[j];
					
					//note that we stuff the oam index into [6]. 
					//i need to turn this into a struct so we can have fewer magic numbers
					oams[scanslot][oamcount][6] = (uint8)i;
					oamcount++;
				}
			}
			oamcounts[scanslot] = oamcount;

			//FV is clocked by the PPU's horizontal blanking impulse, and therefore will increment every scanline.
			//well, according to (which?) tests, maybe at the end of hblank. 
			//but, according to what it took to get crystalis working, it is at the beginning of hblank.
			if(PPUON && sl != 0)
				ppur.increment_vs();

			//todo - think about clearing oams to a predefined value to force deterministic behavior

			//fetch sprite patterns
			for(int s=0;s<8;s++) {

				uint8* oam = oams[scanslot][s];
				uint32 line = yp - oam[0];
				if(oam[2]&0x80) //vflip
					line = spriteHeight-line-1;

				uint32 patternNumber = oam[1];
				uint32 patternAddress;

				//8x16 sprite handling:
				if(Sprite16) {
					uint32 bank = (patternNumber&1)<<12;
					patternNumber = patternNumber&~1;
					patternNumber |= (line>>3);
					patternAddress = (patternNumber<<4) | bank;
				} else {
					patternAddress = (patternNumber<<4) | (SpAdrHI<<9);
				}

				//offset into the pattern for the current line.
				//tricky: tall sprites have already had lines>8 taken care of by getting a new pattern number above.
				//so we just need the line offset for the second pattern
				patternAddress += line&7;

				//garbage nametable fetches
				runppu(kFetchTime);

				if(((PPU[0]&0x38)!=0x18) && s == 2 && SpriteON ) {
					//(The MMC3 scanline counter is based entirely on PPU A12, triggered on rising edges (after the line remains low for a sufficiently long period of time))
					//http://nesdevwiki.org/wiki/index.php/Nintendo_MMC3
					//test cases for timing: SMB3, Crystalis
					//crystalis requires deferring this til somewhere in sprite [1,3]
					//kirby requires deferring this til somewhere in sprite [2,5..
					if(PPUON && GameHBIRQHook) {
						GameHBIRQHook();
					}
				}

				runppu(kFetchTime);

				//pattern table fetches
				RefreshAddr = patternAddress;
				oam[4] = FFCEUX_PPURead(RefreshAddr);
				runppu(kFetchTime);
				RefreshAddr += 8;
				oam[5] = FFCEUX_PPURead(RefreshAddr);
				runppu(kFetchTime);


				//hflip
				if(!(oam[2]&0x40)) {
					oam[4] = bitrevlut[oam[4]];
					oam[5] = bitrevlut[oam[5]];
				}
			}



			//so.. this is the end of hblank. latch horizontal scroll values
			if(PPUON && sl != 0)
				ppur.install_h_latches();

			//capture the next xscroll
			xscroll = ppur.fh;

			//fetch BG: two tiles for next line
			for(int xt=0;xt<2;xt++)
				bgdata.main[xt].Read();

			//I'm unclear of the reason why this particular access to memory is made. 
			//The nametable address that is accessed 2 times in a row here, is also the 
			//same nametable address that points to the 3rd tile to be rendered on the 
			//screen (or basically, the first nametable address that will be accessed when 
			//the PPU is fetching background data on the next scanline).
			//(not implemented yet)
			runppu(kFetchTime);
			runppu(kFetchTime);


			//After memory access 170, the PPU simply rests for 4 cycles (or the 
			//equivelant of half a memory access cycle) before repeating the whole 
			//pixel/scanline rendering process. If the scanline being rendered is the very 
			//first one on every second frame, then this delay simply doesn't exist.
			if(sl==0 && idleSynch==0)
			{}
			else 
				runppu(4);
		}
		
		idleSynch ++;
		if(idleSynch==2) idleSynch = 0;

		//idle for one line
		//why 1360? see the note up top at vblank
		runppu(1360);

		framectr++;

	}

finish:
	FCEU_PutImage();

	return 0;
}
