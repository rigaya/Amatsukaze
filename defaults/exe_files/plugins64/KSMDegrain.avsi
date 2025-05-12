
function KSMDegrain_MAnalyze( clip srchSuper, clip srchSuperP, bool isb, int delta, int blksize, int overlap, \
                    int search, bool truemotion, bool chroma, int batch)
{
    # partial mv on CPU
    pmv = srchSuperP.KMAnalyse(blksize=blksize, overlap=overlap, search=search, isb=isb, chroma=chroma, \
                    delta=delta, meander=false, batch=1).OnCPU(1)
 
    # main mv on CUDA
    return srchSuper.KMAnalyse(blksize=blksize, overlap=overlap, search=search, isb=isb, chroma=chroma, \
                    delta=delta, meander=false, partial=pmv, batch=batch)
}
 
function KSMDegrain(clip c, int "tr", int "thSAD", int "thSADC", int "pel", int "blksize", bool "chroma", int "thSCD1", int "thSCD2", int "limit", int "limitC", bool "binomial", int "dev")
{
    tr = default(tr, 2)
    thSAD = default(thSAD, 300)
    thSADC = default(thSADC, 150)
    pel = default(pel, 1)
    blksize = default(blksize, 16)
    chroma = default(chroma, True)
    thSCD1 = default(thSCD1, 1600)
    thSCD2 = default(thSCD2, 130)
    limit = default(limit, 255)
    limitC = default(limitC, 255)
    binomial = default(binomial, False)
    dev = default(dev, 0)
    
    search = 4
    overlap = blksize / 2
    batch = 4
    
    super = c.KTGMC_ToFullRange(U=3,V=3).KMSuper(pel=pel,chroma=True)
    psuper = super.KMPartialSuper().OnCUDA(1, dev)
    dsuper = c.KMSuper(pel=pel,chroma=True,levels=1)
    bvec2 = (tr >= 2) ? super.KSMDegrain_MAnalyze(psuper, True,2,blksize,overlap,search,False,chroma,batch) : NOP()
    bvec1 = super.KSMDegrain_MAnalyze(psuper, True,1,blksize,overlap,search,False,chroma,batch)
    fvec1 = super.KSMDegrain_MAnalyze(psuper, False,1,blksize,overlap,search,False,chroma,batch)
    fvec2 = (tr >= 2) ? super.KSMDegrain_MAnalyze(psuper, False,2,blksize,overlap,search,False,chroma,batch) : NOP()
    return (tr == 2) ? c.KMDegrain2(dsuper, bvec1, fvec1, bvec2, fvec2, thSAD=thSAD,thSADC=thSADC,thSCD1=thSCD1,thSCD2=thSCD2,limit=limit,limitC=limitC,binomial=binomial) \
        : c.KMDegrain1(dsuper, bvec1, fvec1, thSAD=thSAD,thSADC=thSADC,thSCD1=thSCD1,thSCD2=thSCD2,limit=limit,limitC=limitC,binomial=binomial)
}
