

#if 0

PCIHDR.H: PCI Vendors, Devices, and Class Type information

Created automatically from the web using the following URL:
http://pcidatabase.com/
Software to create and maintain the PCICODE List written by:
Jim Boemler (jboemler@halcyon.com)

  This header created on Fri Jan 16 09:49:40 PST 2009

Too many people have contributed to this list to acknowledge them all, but
a few have provided the majority of the input and deserve special mention:
   Frederic Potter, who maintains a list for Linux.
   Chris Aston at Madge Networks.
   Thomas Dippon of Hewlett-Packard GmbH.
   Jurgen ("Josh") Thelen
   William H. Avery III at Altitech
   Sergei Shtylyov of Brain-dead Software in Russia
#endif

#ifndef PCI_LIST_H
#define PCI_LIST_H

#include <mm.h>

//  NOTE that the 0xFFFF of 0xFF entries at the end of some tables below are
//  not properly list terminators, but are actually the printable definitions
//  of values that are legitimately found on the PCI bus.  The size
//  definitions should be used for loop control when the table is searched.

typedef struct _PCI_VENTABLE
{
	unsigned short	VenId ;
	const char *	VenShort ;
	const char *	VenFull ;
}  PCI_VENTABLE, *PPCI_VENTABLE ;

extern PCI_VENTABLE PciVenTable[];
extern int PCI_VENTABLE_LEN;

typedef struct _PCI_DEVTABLE
{
	unsigned short	VenId ;
	unsigned short	DevId ;
	const char *	Chip ;
	const char *	ChipDesc ;
}  PCI_DEVTABLE, *PPCI_DEVTABLE ;

extern PCI_DEVTABLE PciDevTable[];
extern int PCI_DEVTABLE_LEN;

typedef struct _PCI_CLASSCODETABLE
{
	unsigned char	BaseClass ;
	unsigned char	SubClass ;
	unsigned char	ProgIf ;
	const char *		BaseDesc ;
	const char *		SubDesc ;
	const char *		ProgDesc ;
}  PCI_CLASSCODETABLE, *PPCI_CLASSCODETABLE ;

extern PCI_CLASSCODETABLE PciClassCodeTable[];
extern int PCI_CLASSCODETABLE_LEN;

extern const char * PciCommandFlags[];
extern int PCI_COMMANDFLAGS_LEN;

extern const char * PciStatusFlags[];
extern int PCI_STATUSFLAGS_LEN;

extern const char * PciDevSelFlags[];
extern int PCI_DEVSELFLAGS_LEN;

#endif


