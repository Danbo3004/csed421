/******************************************************************************/
/*                                                                            */
/*    ODYSSEUS/EduCOSMOS Educational-Purpose Object Storage System            */
/*                                                                            */
/*    Developed by Professor Kyu-Young Whang et al.                           */
/*                                                                            */
/*    Database and Multimedia Laboratory                                      */
/*                                                                            */
/*    Computer Science Department and                                         */
/*    Advanced Information Technology Research Center (AITrc)                 */
/*    Korea Advanced Institute of Science and Technology (KAIST)              */
/*                                                                            */
/*    e-mail: kywhang@cs.kaist.ac.kr                                          */
/*    phone: +82-42-350-7722                                                  */
/*    fax: +82-42-350-8380                                                    */
/*                                                                            */
/*    Copyright (c) 1995-2013 by Kyu-Young Whang                              */
/*                                                                            */
/*    All rights reserved. No part of this software may be reproduced,        */
/*    stored in a retrieval system, or transmitted, in any form or by any     */
/*    means, electronic, mechanical, photocopying, recording, or otherwise,   */
/*    without prior written permission of the copyright owner.                */
/*                                                                            */
/******************************************************************************/
/*
 * Module : EduOM_CreateObject.c
 * 
 * Description :
 *  EduOM_CreateObject() creates a new object near the specified object.
 *
 * Exports:
 *  Four EduOM_CreateObject(ObjectID*, ObjectID*, ObjectHdr*, Four, char*, ObjectID*)
 */

#include <string.h>
#include "EduOM_common.h"
#include "RDsM.h" /* for the raw disk manager call */
#include "BfM.h"  /* for the buffer manager call */
#include "EduOM_Internal.h"

/*@================================
 * EduOM_CreateObject()
 *================================*/
/*
 * Function: Four EduOM_CreateObject(ObjectID*, ObjectID*, ObjectHdr*, Four, char*, ObjectID*)
 * 
 * Description :
 * (Following description is for original ODYSSEUS/COSMOS OM.
 *  For ODYSSEUS/EduCOSMOS EduOM, refer to the EduOM project manual.)
 *
 * (1) What to do?
 * EduOM_CreateObject() creates a new object near the specified object.
 * If there is no room in the page holding the specified object,
 * it trys to insert into the page in the available space list. If fail, then
 * the new object will be put into the newly allocated page.
 *
 * (2) How to do?
 *	a. Read in the near slotted page
 *	b. See the object header
 *	c. IF large object THEN
 *	       call the large object manager's lom_ReadObject()
 *	   ELSE 
 *		   IF moved object THEN 
 *				call this function recursively
 *		   ELSE 
 *				copy the data into the buffer
 *		   ENDIF
 *	   ENDIF
 *	d. Free the buffer page
 *	e. Return
 *
 * Returns:
 *  error code
 *    eBADCATALOGOBJECT_OM
 *    eBADLENGTH_OM
 *    eBADUSERBUF_OM
 *    some error codes from the lower level
 *
 * Side Effects :
 *  0) A new object is created.
 *  1) parameter oid
 *     'oid' is set to the ObjectID of the newly created object.
 */
Four EduOM_CreateObject(
    ObjectID *catObjForFile, /* IN file in which object is to be placed */
    ObjectID *nearObj,       /* IN create the new object near this object */
    ObjectHdr *objHdr,       /* IN from which tag is to be set */
    Four length,             /* IN amount of data */
    char *data,              /* IN the initial data for the object */
    ObjectID *oid)           /* OUT the object's ObjectID */
{
    Four e;              /* error number */
    ObjectHdr objectHdr; /* ObjectHdr with tag set from parameter */

    /*@ parameter checking */

    if (catObjForFile == NULL)
        ERR(eBADCATALOGOBJECT_OM);

    if (length < 0)
        ERR(eBADLENGTH_OM);

    if (length > 0 && data == NULL)
        return (eBADUSERBUF_OM);

    /* Error check whether using not supported functionality by EduOM */
    if (ALIGNED_LENGTH(length) > LRGOBJ_THRESHOLD)
        ERR(eNOTSUPPORTED_EDUOM);

    objectHdr.properties = 0;
    objectHdr.length = 0;

    if (objHdr != NULL)
        objectHdr.tag = objHdr->tag;
    else
        objectHdr.tag = 0;

    eduom_CreateObject(catObjForFile, nearObj, &objectHdr, length, data, oid);

    return (eNOERROR);
}

/*@================================
 * eduom_CreateObject()
 *================================*/
/*
 * Function: Four eduom_CreateObject(ObjectID*, ObjectID*, ObjectHdr*, Four, char*, ObjectID*)
 *
 * Description :
 * (Following description is for original ODYSSEUS/COSMOS OM.
 *  For ODYSSEUS/EduCOSMOS EduOM, refer to the EduOM project manual.)
 *
 *  eduom_CreateObject() creates a new object near the specified object; the near
 *  page is the page holding the near object.
 *  If there is no room in the near page and the near object 'nearObj' is not
 *  NULL, a new page is allocated for object creation (In this case, the newly
 *  allocated page is inserted after the near page in the list of pages
 *  consiting in the file).
 *  If there is no room in the near page and the near object 'nearObj' is NULL,
 *  it trys to create a new object in the page in the available space list. If
 *  fail, then the new object will be put into the newly allocated page(In this
 *  case, the newly allocated page is appended at the tail of the list of pages
 *  cosisting in the file).
 *
 * Returns:
 *  error Code
 *    eBADCATALOGOBJECT_OM
 *    eBADOBJECTID_OM
 *    some errors caused by fuction calls
 */
Four eduom_CreateObject(
    ObjectID *catObjForFile, /* IN file in which object is to be placed */
    ObjectID *nearObj,       /* IN create the new object near this object */
    ObjectHdr *objHdr,       /* IN from which tag & properties are set */
    Four length,             /* IN amount of data */
    char *data,              /* IN the initial data for the object */
    ObjectID *oid)           /* OUT the object's ObjectID */
{
    Four e;                  /* error number */
    Four neededSpace;        /* space needed to put new object [+ header] */
    SlottedPage *apage;      /* pointer to the slotted page buffer */
    Four alignedLen;         /* aligned length of initial data */
    Boolean needToAllocPage; /* Is there a need to alloc a new page? */
    PageID pid;              /* PageID in which new object to be inserted */
    PageID nearPid;
    Four firstExt;                  /* first Extent No of the file */
    Object *obj;                    /* point to the newly created object */
    Two i;                          /* index variable */
    sm_CatOverlayForData *catEntry; /* pointer to data file catalog information */
    SlottedPage *catPage;           /* pointer to buffer containing the catalog */
    FileID fid;                     /* ID of file where the new object is placed */
    Two eff;                        /* extent fill factor of file */
    Boolean isTmp;
    PhysicalFileID pFid;
    PageNo *availPage = NIL; //selected page
    SlottedPageSlot *pslot;  //insert slot

    /*@ parameter checking */

    if (catObjForFile == NULL)
        ERR(eBADCATALOGOBJECT_OM);

    if (objHdr == NULL)
        ERR(eBADOBJECTID_OM);

    /* Error check whether using not supported functionality by EduOM */
    if (ALIGNED_LENGTH(length) > LRGOBJ_THRESHOLD)
        ERR(eNOTSUPPORTED_EDUOM);

    // Get the aligned length of the object
    alignedLen = ALIGNED_LENGTH(length);

    // Calculate the space needed for the new object
    neededSpace = sizeof(ObjectHdr) + alignedLen + sizeof(SlottedPageSlot);

    // Get the first extent number of the file
    e = BfM_GetTrain((TrainID *)catObjForFile, (char **)&catPage, PAGE_BUF);
    GET_PTR_TO_CATENTRY_FOR_DATA(catObjForFile, catPage, catEntry);
    MAKE_PHYSICALFILEID(pFid, catEntry->fid.volNo, catEntry->firstPage);
    e = RDsM_PageIdToExtNo((PageID *)&pFid, &firstExt);
    if (e < 0)
        ERR(e);

    // Select a page to insert the object
    if (nearObj != NULL)
    {
        MAKE_PAGEID(nearPid, nearObj->volNo, nearObj->pageNo);
        e = BfM_GetTrain((TrainID *)&nearPid, (char **)&apage, PAGE_BUF);
        if (e < 0)
            ERR(e);
        // Insert the page if fit, otherwise allocate and initialize a new page
        if (neededSpace <= SP_FREE(apage))
        {
            pid = nearPid;
            e = om_RemoveFromAvailSpaceList(catObjForFile, &pid, apage);
            if (e < 0)
                ERRB1(e, &pid, PAGE_BUF);
            if (neededSpace > SP_CFREE(apage))
            {
                e = EduOM_CompactPage(apage, nearObj->slotNo);
                if (e < 0)
                    ERR(e);
            }
        }
        else
        {
            e = BfM_FreeTrain((TrainID *)&nearPid, PAGE_BUF);
            if (e < 0)
                ERR(e);
            e = RDsM_AllocTrains(catEntry->fid.volNo, firstExt, &nearPid, catEntry->eff, 1, PAGESIZE2, &pid);
            if (e < 0)
                ERR(e);
            e = BfM_GetNewTrain((TrainID *)&pid, (char **)&apage, PAGE_BUF);
            if (e < 0)
                ERR(e);

            apage->header.flags = 0x2;
            apage->header.free = 0;
            apage->header.unused = 0;
            apage->header.fid = catEntry->fid;

            e = om_FileMapAddPage(catObjForFile, &nearPid, &pid);
            if (e < 0)
                ERRB1(e, &pid, PAGE_BUF);
        }
    }
    else
    { // NULL nearObj
        // Select a page if there exist a smallest available space that fit
        if (neededSpace <= SP_10SIZE)
            availPage = catEntry->availSpaceList10;
        else if (neededSpace <= SP_20SIZE)
            availPage = catEntry->availSpaceList20;
        else if (neededSpace <= SP_30SIZE)
            availPage = catEntry->availSpaceList30;
        else if (neededSpace <= SP_40SIZE)
            availPage = catEntry->availSpaceList40;
        else if (neededSpace <= SP_50SIZE)
            availPage = catEntry->availSpaceList50;

        // If there is such page, insert
        if (availPage != NIL)
        {
            MAKE_PAGEID(pid, pFid.volNo, availPage);
            e = BfM_GetNewTrain((TrainID *)&pid, (char **)&apage, PAGE_BUF);
            if (e < 0)
                ERR(e);
            e = om_RemoveFromAvailSpaceList(catObjForFile, &pid, apage);
            if (e < 0)
                ERR(e);
            if (neededSpace > SP_CFREE(apage))
            {
                e = EduOM_CompactPage(apage, nearObj->slotNo);
                if (e < 0)
                    ERR(e);
            }
        }
        else
        { // Otherwise, checkout the file last page
            MAKE_PAGEID(pid, pFid.volNo, catEntry->lastPage);
            e = BfM_GetTrain((TrainID *)&pid, (char **)&apage, PAGE_BUF);
            if (e < 0)
                ERR(e);
            if (neededSpace <= SP_FREE(apage))
            {
                e = om_RemoveFromAvailSpaceList(catObjForFile, &pid, apage);
                if (e < 0)
                    ERR(e);
                if (neededSpace > SP_CFREE(apage))
                {
                    e = EduOM_CompactPage(apage, nearObj->slotNo);
                    if (e < 0)
                        ERR(e);
                }
            }
            else
            { // No hope then allocate a new page
                e = BfM_FreeTrain((TrainID *)&pid, PAGE_BUF);
                if (e < 0)
                    ERR(e);
                MAKE_PAGEID(nearPid, pFid.volNo, catEntry->lastPage);
                e = RDsM_AllocTrains(catEntry->fid.volNo, firstExt, &nearPid, catEntry->eff, 1, PAGESIZE2, &pid);
                if (e < 0)
                    ERR(e);
                apage->header.flags = 0x2;
                apage->header.free = 0;
                apage->header.unused = 0;
                apage->header.fid = catEntry->fid;
                e = om_FileMapAddPage(catObjForFile, &nearPid, &pid);
                if (e < 0)
                    ERRB1(e, &pid, PAGE_BUF);
            }
        }
    }

    // Insert the object the selected page
    i = 0;
    while (i < apage->header.nSlots)
    {
        if (apage->slot[-i].offset == EMPTYSLOT)
            break;
        i++;
    }

    pslot = (SlottedPageSlot *)&(apage->slot[-i]);
    pslot->offset = apage->header.free;
    apage->header.free += sizeof(ObjectHdr) + alignedLen;
    obj = (Object *)&(apage->data[pslot->offset]);
    obj->header.properties = objHdr->properties;
    obj->header.tag = objHdr->tag;
    obj->header.length = length;
    memcpy(obj->data, data, length);
    e = om_GetUnique(&pid, &(pslot->unique));
    if (e < 0)
        ERR(e);
    MAKE_OBJECTID(*oid, pid.volNo, pid.pageNo, i, pslot->unique);
    apage->header.nSlots = MAX(apage->header.nSlots, i + 1);

    e = BfM_SetDirty((TrainID *)&pid, PAGE_BUF);
    if (e < 0)
        ERR(e);

    e = BfM_FreeTrain((TrainID *)&pid, PAGE_BUF);
    if (e < 0)
        ERR(e);

    e = om_PutInAvailSpaceList(catObjForFile, &pid, apage);
    if (e < 0)
        ERRB1(e, &pid, PAGE_BUF);

    e = BfM_FreeTrain((TrainID *)catObjForFile, PAGE_BUF);
    if (e < 0)
        ERR(e);

    return (eNOERROR);

} /* eduom_CreateObject() */
