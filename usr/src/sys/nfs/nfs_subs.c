/*
 * Copyright (c) 1989 The Regents of the University of California.
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Rick Macklem at The University of Guelph.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that the above copyright notice and this paragraph are
 * duplicated in all such forms and that any documentation,
 * advertising materials, and other materials related to such
 * distribution and use acknowledge that the software was developed
 * by the University of California, Berkeley.  The name of the
 * University may not be used to endorse or promote products derived
 * from this software without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 *	@(#)nfs_subs.c	7.2 (Berkeley) %G%
 */

/*
 * These functions support the macros and help fiddle mbuf chains for
 * the nfs op functions. They do things like create the rpc header and
 * copy data between mbuf chains and uio lists.
 */
#include "strings.h"
#include "types.h"
#include "param.h"
#include "mount.h"
#include "dir.h"
#include "time.h"
#include "errno.h"
#include "kernel.h"
#include "malloc.h"
#include "mbuf.h"
#include "file.h"
#include "vnode.h"
#include "uio.h"
#include "namei.h"
#include "ucred.h"
#include "rpcv2.h"
#include "nfsv2.h"
#include "nfsnode.h"
#include "nfs.h"
#include "xdr_subs.h"
#include "nfsm_subs.h"

#define TRUE	1
#define	FALSE	0

/*
 * Data items converted to xdr at startup, since they are constant
 * This is kinda hokey, but may save a little time doing byte swaps
 */
u_long nfs_procids[NFS_NPROCS];
u_long nfs_xdrneg1;
u_long rpc_call, rpc_vers, rpc_reply, rpc_msgdenied,
	rpc_mismatch, rpc_auth_unix, rpc_msgaccepted;
u_long nfs_vers, nfs_prog, nfs_true, nfs_false;
/* And other global data */
static u_long *rpc_uidp = (u_long *)0;
static u_long nfs_xid = 1;
static char *rpc_unixauth;
extern long hostid;
extern enum vtype v_type[NFLNK+1];

/* Function ret types */
static char *nfs_unixauth();

/*
 * Create the header for an rpc request packet
 * The function nfs_unixauth() creates a unix style authorization string
 * and returns a ptr to it.
 * The hsiz is the size of the rest of the nfs request header.
 * (just used to decide if a cluster is a good idea)
 * nb: Note that the prog, vers and proc args are already in xdr byte order
 */
struct mbuf *nfsm_reqh(prog, vers, proc, cred, hsiz, bpos, mb, retxid)
	u_long prog;
	u_long vers;
	u_long proc;
	struct ucred *cred;
	int hsiz;
	caddr_t *bpos;
	struct mbuf **mb;
	u_long *retxid;
{
	register struct mbuf *mreq, *m;
	register u_long *p;
	struct mbuf *m1;
	char *ap;
	int asiz, siz;

	NFSMGETHDR(mreq);
	asiz = (((cred->cr_ngroups > 10) ? 10 : cred->cr_ngroups)<<2);
	asiz += nfsm_rndup(hostnamelen)+(9*NFSX_UNSIGNED);

	/* If we need a lot, alloc a cluster ?? */
	if ((asiz+hsiz+RPC_SIZ) > MHLEN)
		NFSMCLGET(mreq, M_WAIT);
	mreq->m_len = NFSMSIZ(mreq);
	siz = mreq->m_len;
	m1 = mreq;
	/*
	 * Alloc enough mbufs
	 * We do it now to avoid all sleeps after the call to nfs_unixauth()
	 */
	while ((asiz+RPC_SIZ) > siz) {
		MGET(m, M_WAIT, MT_DATA);
		m1->m_next = m;
		m->m_len = MLEN;
		siz += MLEN;
		m1 = m;
	}
	p = mtod(mreq, u_long *);
	*p++ = *retxid = txdr_unsigned(++nfs_xid);
	*p++ = rpc_call;
	*p++ = rpc_vers;
	*p++ = prog;
	*p++ = vers;
	*p++ = proc;

	/* Now we can call nfs_unixauth() and copy it in */
	ap = nfs_unixauth(cred);
	m = mreq;
	siz = m->m_len-RPC_SIZ;
	if (asiz <= siz) {
		bcopy(ap, (caddr_t)p, asiz);
		m->m_len = asiz+RPC_SIZ;
	} else {
		bcopy(ap, (caddr_t)p, siz);
		ap += siz;
		asiz -= siz;
		while (asiz > 0) {
			siz = (asiz > MLEN) ? MLEN : asiz;
			m = m->m_next;
			bcopy(ap, mtod(m, caddr_t), siz);
			m->m_len = siz;
			asiz -= siz;
			ap += siz;
		}
	}
	
	/* Finally, return values */
	*mb = m;
	*bpos = mtod(m, caddr_t)+m->m_len;
	return (mreq);
}

/*
 * copies mbuf chain to the uio scatter/gather list
 */
nfsm_mbuftouio(mrep, uiop, siz, dpos)
	struct mbuf **mrep;
	struct uio *uiop;
	int siz;
	caddr_t *dpos;
{
	register int xfer, left, len;
	register struct mbuf *mp;
	register char *mbufcp, *uiocp;
	long uiosiz, rem;

	mp = *mrep;
	mbufcp = *dpos;
	len = mtod(mp, caddr_t)+mp->m_len-mbufcp;
	rem = nfsm_rndup(siz)-siz;
	while (siz > 0) {
		if (uiop->uio_iovcnt <= 0 || uiop->uio_iov == NULL)
			return(EFBIG);
		left = uiop->uio_iov->iov_len;
		uiocp = uiop->uio_iov->iov_base;
		if (left > siz)
			left = siz;
		uiosiz = left;
		while (left > 0) {
			while (len == 0) {
				mp = mp->m_next;
				if (mp == NULL)
					return (EBADRPC);
				mbufcp = mtod(mp, caddr_t);
				len = mp->m_len;
			}
			xfer = (left > len) ? len : left;
#ifdef notdef
			/* Not Yet.. */
			if (uiop->uio_iov->iov_op != NULL)
				(*(uiop->uio_iov->iov_op))
				(mbufcp, uiocp, xfer);
			else
#endif
			if (uiop->uio_segflg == UIO_SYSSPACE)
				bcopy(mbufcp, uiocp, xfer);
			else
				copyout(mbufcp, uiocp, xfer);
			left -= xfer;
			len -= xfer;
			mbufcp += xfer;
			uiocp += xfer;
			uiop->uio_resid -= xfer;
		}
		if (uiop->uio_iov->iov_len <= siz) {
			uiop->uio_iovcnt--;
			uiop->uio_iov++;
		} else {
			uiop->uio_iov->iov_base += uiosiz;
			uiop->uio_iov->iov_len -= uiosiz;
		}
		siz -= uiosiz;
	}
	if (rem > 0)
		mbufcp += rem;
	*dpos = mbufcp;
	*mrep = mp;
	return(0);
}

/*
 * copies a uio scatter/gather list to an mbuf chain...
 */
nfsm_uiotombuf(uiop, mq, siz, bpos)
	register struct uio *uiop;
	struct mbuf **mq;
	int siz;
	caddr_t *bpos;
{
	register struct mbuf *mp;
	struct mbuf *mp2;
	long xfer, left, uiosiz, off;
	int clflg;
	int rem, len;
	char *cp, *uiocp;

	if (siz > MLEN)		/* or should it >= MCLBYTES ?? */
		clflg = 1;
	else
		clflg = 0;
	rem = nfsm_rndup(siz)-siz;
	mp2 = *mq;
	while (siz > 0) {
		if (uiop->uio_iovcnt <= 0 || uiop->uio_iov == NULL)
			return(EINVAL);
		left = uiop->uio_iov->iov_len;
		uiocp = uiop->uio_iov->iov_base;
		if (left > siz)
			left = siz;
		uiosiz = left;
		while (left > 0) {
			MGET(mp, M_WAIT, MT_DATA);
			if (clflg)
				NFSMCLGET(mp, M_WAIT);
			mp->m_len = NFSMSIZ(mp);
			mp2->m_next = mp;
			mp2 = mp;
			xfer = (left > mp->m_len) ? mp->m_len : left;
#ifdef notdef
			/* Not Yet.. */
			if (uiop->uio_iov->iov_op != NULL)
				(*(uiop->uio_iov->iov_op))
				(uiocp, mtod(mp, caddr_t), xfer);
			else
#endif
			if (uiop->uio_segflg == UIO_SYSSPACE)
				bcopy(uiocp, mtod(mp, caddr_t), xfer);
			else
				copyin(uiocp, mtod(mp, caddr_t), xfer);
			len = mp->m_len;
			mp->m_len = xfer;
			left -= xfer;
			uiocp += xfer;
			uiop->uio_resid -= xfer;
		}
		if (uiop->uio_iov->iov_len <= siz) {
			uiop->uio_iovcnt--;
			uiop->uio_iov++;
		} else {
			uiop->uio_iov->iov_base += uiosiz;
			uiop->uio_iov->iov_len -= uiosiz;
		}
		siz -= uiosiz;
	}
	if (rem > 0) {
		if (rem > (len-mp->m_len)) {
			MGET(mp, M_WAIT, MT_DATA);
			mp->m_len = 0;
			mp2->m_next = mp;
		}
		cp = mtod(mp, caddr_t)+mp->m_len;
		for (left = 0; left < rem; left++)
			*cp++ = '\0';
		mp->m_len += rem;
		*bpos = cp;
	} else
		*bpos = mtod(mp, caddr_t)+mp->m_len;
	*mq = mp;
	return(0);
}

/*
 * Help break down an mbuf chain by setting the first siz bytes contiguous
 * pointed to by returned val.
 * If Updateflg == True we can overwrite the first part of the mbuf data
 * This is used by the macros nfsm_disect and nfsm_disecton for tough
 * cases. (The macros use the vars. dpos and dpos2)
 */
nfsm_disct(mdp, dposp, siz, left, updateflg, cp2)
	struct mbuf **mdp;
	caddr_t *dposp;
	int siz;
	int left;
	int updateflg;
	caddr_t *cp2;
{
	register struct mbuf *mp, *mp2;
	register int siz2, xfer;
	register caddr_t p;
	caddr_t p2;

	mp = *mdp;
	while (left == 0) {
		*mdp = mp = mp->m_next;
		if (mp == NULL)
			return(EBADRPC);
		left = mp->m_len;
		*dposp = mtod(mp, caddr_t);
	}
	if (left >= siz) {
		*cp2 = *dposp;
		*dposp += siz;
		return(0);
	} else if (mp->m_next == NULL) {
		return(EBADRPC);
	} else if (siz > MCLBYTES) {
		panic("nfs S too big");
	} else {
		/* Iff update, you can overwrite, else must alloc new mbuf */
		if (updateflg) {
			NFSMINOFF(mp);
		} else {
			MGET(mp2, M_WAIT, MT_DATA);
			mp2->m_next = mp->m_next;
			mp->m_next = mp2;
			mp->m_len -= left;
			mp = mp2;
		}
		/* Alloc cluster iff we need it */
		if (!M_HASCL(mp) && siz > NFSMSIZ(mp)) {
			NFSMCLGET(mp, M_WAIT);
			if (!M_HASCL(mp))
				return(ENOBUFS);
		}
		*cp2 = p = mtod(mp, caddr_t);
		bcopy(*dposp, p, left);		/* Copy what was left */
		siz2 = siz-left;
		p += left;
		mp2 = mp->m_next;
		/* Loop arround copying up the siz2 bytes */
		while (siz2 > 0) {
			if (mp2 == NULL)
				return (EBADRPC);
			xfer = (siz2 > mp2->m_len) ? mp2->m_len : siz2;
			bcopy(mtod(mp2, caddr_t), p, xfer);
			NFSMADV(mp2, xfer);
			mp2->m_len -= xfer;
			siz2 -= xfer;
			if (siz2 > 0)
				mp2 = mp2->m_next;
		}
		mp->m_len = siz;
		*mdp = mp2;
		*dposp = mtod(mp2, caddr_t);
		return (0);
	}
}

/*
 * Advance the position in the mbuf chain with/without freeing mbufs
 */
nfs_adv(mdp, dposp, offs, left)
	struct mbuf **mdp;
	caddr_t *dposp;
	int offs;
	int left;
{
	register struct mbuf *m;
	register int s;

	m = *mdp;
	s = left;
	while (s < offs) {
		offs -= s;
		m = m->m_next;
		if (m == NULL)
			return(EBADRPC);
		s = m->m_len;
	}
	*mdp = m;
	*dposp = mtod(m, caddr_t)+offs;
	return(0);
}

/*
 * Copy a string into mbufs for the hard cases...
 */
nfsm_strtmbuf(mb, bpos, cp, siz)
	struct mbuf **mb;
	char **bpos;
	char *cp;
	long siz;
{
	register struct mbuf *m1, *m2;
	long left, xfer, len, tlen;
	u_long *p;
	int putsize;

	putsize = 1;
	m2 = *mb;
	left = NFSMSIZ(m2)-m2->m_len;
	if (left > 0) {
		p = ((u_long *)(*bpos));
		*p++ = txdr_unsigned(siz);
		putsize = 0;
		left -= NFSX_UNSIGNED;
		m2->m_len += NFSX_UNSIGNED;
		if (left > 0) {
			bcopy(cp, (caddr_t) p, left);
			siz -= left;
			cp += left;
			m2->m_len += left;
			left = 0;
		}
	}
	/* Loop arround adding mbufs */
	while (siz > 0) {
		MGET(m1, M_WAIT, MT_DATA);
		if (siz > MLEN)
			NFSMCLGET(m1, M_WAIT);
		m1->m_len = NFSMSIZ(m1);
		m2->m_next = m1;
		m2 = m1;
		p = mtod(m1, u_long *);
		tlen = 0;
		if (putsize) {
			*p++ = txdr_unsigned(siz);
			m1->m_len -= NFSX_UNSIGNED;
			tlen = NFSX_UNSIGNED;
			putsize = 0;
		}
		if (siz < m1->m_len) {
			len = nfsm_rndup(siz);
			xfer = siz;
			if (xfer < len)
				*(p+(xfer>>2)) = 0;
		} else {
			xfer = len = m1->m_len;
		}
		bcopy(cp, (caddr_t) p, xfer);
		m1->m_len = len+tlen;
		siz -= xfer;
		cp += xfer;
	}
	*mb = m1;
	*bpos = mtod(m1, caddr_t)+m1->m_len;
	return(0);
}

/*
 * Called once to initialize data structures...
 */
nfsinit()
{
	register int i;

	rpc_vers = txdr_unsigned(RPC_VER2);
	rpc_call = txdr_unsigned(RPC_CALL);
	rpc_reply = txdr_unsigned(RPC_REPLY);
	rpc_msgdenied = txdr_unsigned(RPC_MSGDENIED);
	rpc_msgaccepted = txdr_unsigned(RPC_MSGACCEPTED);
	rpc_mismatch = txdr_unsigned(RPC_MISMATCH);
	rpc_auth_unix = txdr_unsigned(RPCAUTH_UNIX);
	nfs_vers = txdr_unsigned(NFS_VER2);
	nfs_prog = txdr_unsigned(NFS_PROG);
	nfs_true = txdr_unsigned(TRUE);
	nfs_false = txdr_unsigned(FALSE);
	/* Loop thru nfs procids */
	for (i = 0; i < NFS_NPROCS; i++)
		nfs_procids[i] = txdr_unsigned(i);
	v_type[0] = VNON;
	v_type[1] = VREG;
	v_type[2] = VDIR;
	v_type[3] = VBLK;
	v_type[4] = VCHR;
	v_type[5] = VLNK;
	nfs_xdrneg1 = txdr_unsigned(-1);
	nfs_nhinit();			/* Init the nfsnode table */
	/* And start timer */
	nfs_timer();
}

/*
 * Fill in the rest of the rpc_unixauth and return it
 */
static char *nfs_unixauth(cr)
	register struct ucred *cr;
{
	register u_long *p;
	register int i;
	int ngr;

	/* Maybe someday there should be a cache of AUTH_SHORT's */
	if ((p = rpc_uidp) == NULL) {
		i = nfsm_rndup(hostnamelen)+(19*NFSX_UNSIGNED);
		MALLOC(p, u_long *, i, M_TEMP, M_WAITOK);
		bzero((caddr_t)p, i);
		rpc_unixauth = (caddr_t)p;
		*p++ = txdr_unsigned(RPCAUTH_UNIX);
		p++;	/* Fill in size later */
		*p++ = hostid;
		*p++ = txdr_unsigned(hostnamelen);
		i = nfsm_rndup(hostnamelen);
		bcopy(hostname, (caddr_t)p, hostnamelen);
		p += (i>>2);
		rpc_uidp = p;
	}
	*p++ = txdr_unsigned(cr->cr_uid);
	*p++ = txdr_unsigned(cr->cr_groups[0]);
	ngr = (cr->cr_ngroups > 10) ? 10 : cr->cr_ngroups;
	*p++ = txdr_unsigned(ngr);
	for (i = 0; i < ngr; i++)
		*p++ = txdr_unsigned(cr->cr_groups[i]);
	/* And add the AUTH_NULL */
	*p++ = 0;
	*p = 0;
	i = (((caddr_t)p)-rpc_unixauth)-12;
	p = (u_long *)(rpc_unixauth+4);
	*p = txdr_unsigned(i);
	return(rpc_unixauth);
}

/*
 * Attribute cache routines.
 * nfs_loadattrcache() - loads or updates the cache contents from attributes
 *	that are on the mbuf list
 * nfs_getattrcache() - returns valid attributes if found in cache, returns
 *	error otherwise
 */

/*
 * Load the attribute cache (that lives in the nfsnode table) with
 * the values on the mbuf list and
 * Iff vap not NULL
 *    copy the attributes to *vaper
 */
nfs_loadattrcache(vp, mdp, dposp, vaper)
	register struct vnode *vp;
	struct mbuf **mdp;
	caddr_t *dposp;
	struct vattr *vaper;
{
	register struct vattr *vap;
	nfsm_vars;
	struct nfsnode *np;

	md = *mdp;
	dpos = *dposp;
	t1 = (mtod(md, caddr_t)+md->m_len)-dpos;
	if (error = nfsm_disct(&md, &dpos, NFSX_FATTR, t1, TRUE, &cp2))
		return (error);
	p = (u_long *)cp2;
	np = VTONFS(vp);
	vap = &np->n_vattr;
	vap->va_type = nfstov_type(*p++);
	vap->va_mode = nfstov_mode(*p++);
	vap->va_nlink = fxdr_unsigned(u_short, *p++);
	vap->va_uid = fxdr_unsigned(uid_t, *p++);
	vap->va_gid = fxdr_unsigned(gid_t, *p++);
	vap->va_size = fxdr_unsigned(u_long, *p++);
	vap->va_size1 = 0;		/* OR -1 ?? */
	vap->va_blocksize = fxdr_unsigned(long, *p++);
	vap->va_rdev = fxdr_unsigned(dev_t, *p++);
	vap->va_bytes = fxdr_unsigned(long, *p++);
	vap->va_bytes1 = -1;
	vap->va_fsid = fxdr_unsigned(long, *p++);
	vap->va_fileid = fxdr_unsigned(long, *p++);
	fxdr_time(p, &(vap->va_atime));
	p += 2;
	fxdr_time(p, &(vap->va_mtime));
	p += 2;
	fxdr_time(p, &(vap->va_ctime));
	np->n_attrstamp = time.tv_sec;
	*dposp = dpos;
	*mdp = md;
	if (vaper != NULL)
		bcopy((caddr_t)vap, (caddr_t)vaper, sizeof(*vap));
	return (0);
}

/*
 * Check the time stamp
 * If the cache is valid, copy contents to *vap and return 0
 * otherwise return an error
 */
nfs_getattrcache(vp, vap)
	register struct vnode *vp;
	struct vattr *vap;
{
	register struct nfsnode *np;

	np = VTONFS(vp);
	if ((time.tv_sec-np->n_attrstamp) < NFS_ATTRTIMEO) {
		nfsstats.attrcache_hits++;
		bcopy((caddr_t)&np->n_vattr,(caddr_t)vap,sizeof(struct vattr));
		return (0);
	} else {
		nfsstats.attrcache_misses++;
		return (ENOENT);
	}
}

#ifndef OPFLAG
#define	OPFLAG	(CREATE | DELETE | LOOKUP)
#endif

/*
 * nfs_namei - a liitle like namei(), but for one element only
 *	essentially look up file handle, fill in ndp and call VOP_LOOKUP()
 */
nfs_namei(ndp, fhp, len, mdp, dposp)
	register struct nameidata *ndp;
	fhandle_t *fhp;
	int len;
	struct mbuf **mdp;
	caddr_t *dposp;
{
	register int i, rem;
	register struct mbuf *md;
	register char *cp;
	struct vnode *dp = (struct vnode *)0;
	struct vnode *tdp;
	struct mount *mp;
	int flag;
	int docache;
	int wantparent;
	int lockparent;
	int rootflg = 0;
	int error = 0;

	ndp->ni_vp = ndp->ni_dvp = (struct vnode *)0;
	flag = ndp->ni_nameiop & OPFLAG;
	wantparent = ndp->ni_nameiop & (LOCKPARENT | WANTPARENT);
	lockparent = ndp->ni_nameiop & LOCKPARENT;
	docache = (ndp->ni_nameiop & NOCACHE) ^ NOCACHE;
	if (flag == DELETE || wantparent)
		docache = 0;

	/* Fill in the nameidata and call lookup */
	cp = *dposp;
	md = *mdp;
	rem = mtod(md, caddr_t)+md->m_len-cp;
	ndp->ni_hash = 0;
	for (i = 0; i < len;) {
		if (rem == 0) {
			md = md->m_next;
			if (md == NULL)
				return (EBADRPC);
			cp = mtod(md, caddr_t);
			rem = md->m_len;
		}
		if (*cp == '\0' || *cp == '/')
			return (EINVAL);
		if (*cp & 0200)
			if ((*cp&0377) == ('/'|0200) || flag != DELETE)
				return (EINVAL);
		ndp->ni_dent.d_name[i++] = *cp;
		ndp->ni_hash += (unsigned char)*cp * i;
		cp++;
		rem--;
	}
	*mdp = md;
	len = nfsm_rndup(len)-len;
	if (len > 0)
		*dposp = cp+len;
	else
		*dposp = cp;
	ndp->ni_namelen = i;
	ndp->ni_dent.d_namlen = i;
	ndp->ni_dent.d_name[i] = '\0';
	ndp->ni_pathlen = 0;
	ndp->ni_dirp = ndp->ni_ptr = &ndp->ni_dent.d_name[0];
	ndp->ni_next = &ndp->ni_dent.d_name[i];
	ndp->ni_loopcnt = 0;	/* Not actually used for now */
	ndp->ni_endoff = 0;
	if (docache)
		ndp->ni_makeentry = 1;
	else
		ndp->ni_makeentry = 0;
	ndp->ni_isdotdot = (i == 2 && 
		ndp->ni_dent.d_name[1] == '.' && ndp->ni_dent.d_name[0] == '.');

	/*
	 * Must remember if this is root so that cr_uid can be set to
	 * mp->m_exroot at mount points
	 * Then call nfsrv_fhtovp() to get the locked directory vnode
	 */
	if (ndp->ni_cred->cr_uid == 0)
		rootflg++;
	if (error = nfsrv_fhtovp(fhp, TRUE, &dp, ndp->ni_cred))
		return (error);

	/*
	 * Handle "..": two special cases.
	 * 1. If at root directory (e.g. after chroot)
	 *    then ignore it so can't get out.
	 * 2. If this vnode is the root of a mounted
	 *    file system, then replace it with the
	 *    vnode which was mounted on so we take the
	 *    .. in the other file system.
	 */
	if (ndp->ni_isdotdot) {
		for (;;) {
			if (dp == rootdir) {
				ndp->ni_dvp = dp;
				dp->v_count++;
				goto nextname;
			}
			if ((dp->v_flag & VROOT) == 0)
				break;
			tdp = dp;
			dp = dp->v_mount->m_vnodecovered;
			vput(tdp);
			if ((dp->v_mount->m_flag & M_EXPORTED) == 0)
				return (EACCES);
			VOP_LOCK(dp);
			dp->v_count++;
			if (rootflg)
				ndp->ni_cred->cr_uid = dp->v_mount->m_exroot;
		}
	}

	/*
	 * We now have a segment name to search for, and a directory to search.
	 */
	if (error = VOP_LOOKUP(dp, ndp)) {
		if (ndp->ni_vp != NULL)
			panic("leaf should be empty");
		/*
		 * If creating and at end of pathname, then can consider
		 * allowing file to be created.
		 */
		if (ndp->ni_dvp->v_mount->m_flag & (M_RDONLY | M_EXRDONLY))
			error = EROFS;
		if (flag == LOOKUP || flag == DELETE || error != ENOENT)
			goto bad;
		/*
		 * We return with ni_vp NULL to indicate that the entry
		 * doesn't currently exist, leaving a pointer to the
		 * (possibly locked) directory inode in ndp->ni_dvp.
		 */
		return (0);	/* should this be ENOENT? */
	}

	/*
	 * Check for symbolic link
	 */
	dp = ndp->ni_vp;
#ifdef notdef
	if ((dp->v_type == VLNK) &&
	    (ndp->ni_nameiop & FOLLOW)) {
		struct iovec aiov;
		struct uio auio;
		int linklen;

		if (++ndp->ni_loopcnt > MAXSYMLINKS) {
			error = ELOOP;
			goto bad2;
		}
		MALLOC(cp, char *, MAXPATHLEN, M_NAMEI, M_WAITOK);
		aiov.iov_base = cp;
		aiov.iov_len = MAXPATHLEN;
		auio.uio_iov = &aiov;
		auio.uio_iovcnt = 1;
		auio.uio_offset = 0;
		auio.uio_rw = UIO_READ;
		auio.uio_segflg = UIO_SYSSPACE;
		auio.uio_resid = MAXPATHLEN;
		if (error = VOP_READLINK(dp, &auio, ndp->ni_cred)) {
			free(cp, M_NAMEI);
			goto bad2;
		}
		linklen = MAXPATHLEN - auio.uio_resid;
		if (linklen + ndp->ni_pathlen >= MAXPATHLEN) {
			free(cp, M_NAMEI);
			error = ENAMETOOLONG;
			goto bad2;
		}
		bcopy(ndp->ni_next, cp + linklen, ndp->ni_pathlen);
		ndp->ni_pnbuf = cp;
		} else
			ndp->ni_pnbuf[linklen] = '\0';
		ndp->ni_ptr = cp;
		ndp->ni_pathlen += linklen;
		vput(dp);
		dp = ndp->ni_dvp;
		goto start;
	}
#endif

	/*
	 * Check to see if the vnode has been mounted on;
	 * if so find the root of the mounted file system.
	 * Ignore NFS mount points
	 */
mntloop:
	while (dp->v_type == VDIR && (mp = dp->v_mountedhere) &&
		mp->m_fsid.val[1] != MOUNT_NFS) {
		while(mp->m_flag & M_MLOCK) {
			mp->m_flag |= M_MWAIT;
			sleep((caddr_t)mp, PVFS);
			goto mntloop;
		}
		error = VFS_ROOT(mp, &tdp);
		if (error || (mp->m_flag & M_EXPORTED) == 0)
			goto bad2;
		vput(dp);
		ndp->ni_vp = dp = tdp;
		if (rootflg)
			ndp->ni_cred->cr_uid = mp->m_exroot;
	}

nextname:
	/*
	 * Check for read-only file systems.
	 */
	if (flag == DELETE || flag == RENAME) {
		/*
		 * Disallow directory write attempts on read-only
		 * file systems.
		 */
		if ((dp->v_mount->m_flag & (M_RDONLY|M_EXRDONLY)) ||
		    (wantparent && (ndp->ni_dvp->v_mount->m_flag & M_RDONLY))) {
			error = EROFS;
			goto bad2;
		}
	}

	/*
	 * Kludge city... This is hokey, but since ufs_rename() calls
	 * namei() and namei() expects ni_cdir to be set, what can I
	 * do. Fortunately rename() holds onto the parent so I don't
	 * have to increment the v_count.
	 */
	if (!wantparent)
		vrele(ndp->ni_dvp);
	else
		ndp->ni_cdir = ndp->ni_dvp;

	if ((ndp->ni_nameiop & LOCKLEAF) == 0)
		VOP_UNLOCK(dp);
	return (0);

bad2:
	if (lockparent)
		VOP_UNLOCK(ndp->ni_dvp);
	vrele(ndp->ni_dvp);
bad:
	vput(dp);
	ndp->ni_vp = NULL;
	return (error);
}

/*
 * A fiddled version of m_adj() that ensures null fill to a long
 * boundary and only trims off the back end
 */
nfsm_adj(mp, len, nul)
	struct mbuf *mp;
	register int len;
	int nul;
{
	register struct mbuf *m;
	register int count, i;
	register char *cp;

	/*
	 * Trim from tail.  Scan the mbuf chain,
	 * calculating its length and finding the last mbuf.
	 * If the adjustment only affects this mbuf, then just
	 * adjust and return.  Otherwise, rescan and truncate
	 * after the remaining size.
	 */
	count = 0;
	m = mp;
	for (;;) {
		count += m->m_len;
		if (m->m_next == (struct mbuf *)0)
			break;
		m = m->m_next;
	}
	if (m->m_len >= len) {
		m->m_len -= len;
		if (nul > 0) {
			cp = mtod(m, caddr_t)+m->m_len-nul;
			for (i = 0; i < nul; i++)
				*cp++ = '\0';
		}
		return;
	}
	count -= len;
	if (count < 0)
		count = 0;
	/*
	 * Correct length for chain is "count".
	 * Find the mbuf with last data, adjust its length,
	 * and toss data from remaining mbufs on chain.
	 */
	for (m = mp; m; m = m->m_next) {
		if (m->m_len >= count) {
			m->m_len = count;
			if (nul > 0) {
				cp = mtod(m, caddr_t)+m->m_len-nul;
				for (i = 0; i < nul; i++)
					*cp++ = '\0';
			}
			break;
		}
		count -= m->m_len;
	}
	while (m = m->m_next)
		m->m_len = 0;
}

/*
 * nfsrv_fhtovp() - convert a fh to a vnode ptr (optionally locked)
 * 	- look up fsid in mount list (if not found ret error)
 *	- check that it is exported
 *	- get vp by calling VFS_FHTOVP() macro
 *	- if not lockflag unlock it with VOP_UNLOCK()
 *	- if cred->cr_uid == 0 set it to m_exroot
 */
nfsrv_fhtovp(fhp, lockflag, vpp, cred)
	fhandle_t *fhp;
	int lockflag;
	struct vnode **vpp;
	struct ucred *cred;
{
	register struct mount *mp;
	int error;

	if ((mp = getvfs(&fhp->fh_fsid)) == NULL)
		return (ESTALE);
	if ((mp->m_flag & M_EXPORTED) == 0)
		return (EACCES);
	if (VFS_FHTOVP(mp, &fhp->fh_fid, vpp))
		return (ESTALE);
	if (cred->cr_uid == 0)
		cred->cr_uid = mp->m_exroot;
	if (!lockflag)
		VOP_UNLOCK(*vpp);
	return (0);
}

