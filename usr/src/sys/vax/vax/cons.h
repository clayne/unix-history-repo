/*	cons.h	4.3	%G%	*/

/*
 * VAX console interface registers
 */

#define	RXCS_IE		0x40		/* receiver interrupt enable */
#define	RXCS_DONE	0x80		/* receiver done */
#define	RXDB_DATA	0xff		/* received character */
#define	RXDB_ID		0xf00		/* channel id */
#define	RXDB_ERR	0x80000000	/* receiver error */

#define	TXCS_IE		0x40		/* transmitter interrupt enable */
#define	TXCS_RDY	0x80		/* transmitter ready for next char */
#define	TXDB_DATA	0xff		/* transmitter byte */
#define	TXDB_ID		0xf00		/* channel id */

#if VAX==780
#define	TXDB_WSI	0xf02		/* set warm start inhibit */
#define	TXDB_CWSI	0xf03		/* clear warm start inhibit */
#define	TXDB_BOOT	0xf04		/* enable boot */
#endif
#if VAX==750
#define	TXDB_BOOT	0xf02		/* reboot */
#define	TXDB_CCSF	0xf04		/* clear cold-start flag */
#endif
