/*
 * ClassifierManager.cpp
 *
 *  Created on: Nov 28, 2015
 *      Author: yankai
 */

#include "_Classifier.h"

namespace kai
{

_Classifier::_Classifier()
{
	_ThreadBase();

	int i, j;
	m_numObj = NUM_OBJ;
	m_numBatch = 0;
	m_frameID = 0;
	m_frameLifeTime = 0;
	m_objProbMin = 0;
	m_disparity = 0;

	for (i = 0; i < m_numObj; i++)
	{
		m_pObjects[i].m_frameID = 0;
		m_pObjects[i].m_status = OBJ_VACANT;
		m_pObjects[i].m_vContours.clear();

		for (j = 0; j < NUM_OBJECT_NAME; j++)
		{
			m_pObjects[i].m_name[j] = "";
			m_pObjects[i].m_prob[j] = 0;
		}
	}

}

_Classifier::~_Classifier()
{
	// TODO Auto-generated destructor stub
}

bool _Classifier::init(JSON* pJson)
{
	//Setup Caffe Classifier
	string caffeDir = "";
	string modelFile;
	string trainedFile;
	string meanFile;
	string labelFile;

	CHECK_INFO(pJson->getVal("CAFFE_DIR", &caffeDir));
	CHECK_FATAL(pJson->getVal("CAFFE_MODEL_FILE", &modelFile));
	CHECK_FATAL(pJson->getVal("CAFFE_TRAINED_FILE", &trainedFile));
	CHECK_FATAL(pJson->getVal("CAFFE_MEAN_FILE", &meanFile));
	CHECK_FATAL(pJson->getVal("CAFFE_LABEL_FILE", &labelFile));
	m_caffe.setup(caffeDir+modelFile, caffeDir+trainedFile, caffeDir+meanFile, caffeDir+labelFile, NUM_DETECT_BATCH);
	LOG(INFO)<<"Caffe Initialized";

	CHECK_ERROR(pJson->getVal("CLASSIFIER_FRAME_LIFETIME", &m_frameLifeTime));
	CHECK_ERROR(pJson->getVal("CLASSIFIER_PROB_MIN", &m_objProbMin));
	CHECK_ERROR(pJson->getVal("CLASSIFIER_POS_DISPARITY", &m_disparity));


	double FPS = DEFAULT_FPS;
	CHECK_ERROR(pJson->getVal("CLASSIFIER_FPS", &FPS));
	this->setTargetFPS(FPS);

	return true;
}

bool _Classifier::start(void)
{
	m_bThreadON = true;
	int retCode = pthread_create(&m_threadID, 0, getUpdateThread, this);
	if (retCode != 0)
	{
		LOG(ERROR)<< retCode;
		m_bThreadON = false;
		return false;
	}

	return true;
}

void _Classifier::update(void)
{
	m_caffe.setModeGPU();

	while (m_bThreadON)
	{
		this->autoFPSfrom();

		m_frameID = get_time_usec();

		classifyObject();

		this->autoFPSto();
	}

}

void _Classifier::deleteObject(int i)
{
	OBJECT* pObj = &m_pObjects[i];

	//Follow the order, change the status after releasing m_pMat
	pObj->m_vContours.clear();
	pObj->m_status = OBJ_VACANT;
}

void _Classifier::classifyObject(void)
{
	int i, j;
	OBJECT* pObj;
	int numBatch;
	OBJECT* pObjBatch[NUM_DETECT_BATCH];
	string* pName;

	numBatch = 0;

	//Collect the candidates
	for (i = 0; i < m_numObj; i++)
	{
		pObj = &m_pObjects[i];

		//Delete the outdated frame
		if(m_frameID - pObj->m_frameID > m_frameLifeTime)
		{
			deleteObject(i);
			continue;
		}

		//Delete the uncertain ones
		if(pObj->m_status == OBJ_COMPLETE)
		{
			if(pObj->m_prob[0]<=m_objProbMin)
			{
				deleteObject(i);
				continue;
			}
		}

		if(pObj->m_status != OBJ_ADDED)continue;
		if(pObj->m_Mat.empty())continue;

		//TODO: resize
		pObj->m_status = OBJ_CLASSIFYING;
		m_vMat.push_back(pObj->m_Mat);

		pObjBatch[numBatch] = pObj;

		numBatch++;
		if(numBatch >= NUM_DETECT_BATCH)break;
	}

	if(numBatch <= 0)return;

#ifdef CLASSIFIER_DEBUG
	uint64_t tA,tB;
	tA = get_time_usec();
#endif

	//Get the top 5 possible labels
	m_vPredictions = m_caffe.ClassifyBatch(m_vMat, 5);
	m_vMat.clear();

#ifdef CLASSIFIER_DEBUG
	tB = get_time_usec();
	printf("CAFFE >> TIME:%d\n", tB-tA);
#endif

	for (i = 0; i < numBatch; i++)
	{
		pObj = pObjBatch[i];

		for (j = 0; j < m_vPredictions[i].size(); j++)
		{
			Prediction p = m_vPredictions[i][j];
			pName = &pObj->m_name[j];

			*pName = p.first;
			pObj->m_prob[j] = p.second;

			int from = pName->find_first_of(' ');
			int to = pName->find_first_of(',');

			*pName = pName->substr(from+1, to-from);// pObj->m_name[j].length());

			if (j >= NUM_OBJECT_NAME)break;
		}

		pObj->m_status = OBJ_COMPLETE;
	}

#ifdef CLASSIFIER_DEBUG
	printf("CAFFE >> %s\n", pObj->m_name[0].c_str());
#endif
}

OBJECT* _Classifier::addObject(Mat* pMat, Rect* pRect, vector<Point>* pContour)
{
	if(!pMat)return NULL;
	if(!pRect)return NULL;
	if(pMat->empty())return NULL;
	if(pRect->width<=0)return NULL;
	if(pRect->height<=0)return NULL;

	int i;
	int iVacant;
	OBJECT* pObj;
	uint64_t frameID = get_time_usec();

	iVacant = m_numObj;
	for(i=0; i<m_numObj; i++)
	{
		pObj = &m_pObjects[i];

		//Record the index of vacancy
		if(pObj->m_status == OBJ_VACANT)
		{
			if(iVacant==m_numObj)
			{
				iVacant = i;
			}

			continue;
		}

		//Compare if already existed
		if(abs(pObj->m_boundBox.x - pRect->x) > m_disparity)continue;
		if(abs(pObj->m_boundBox.y - pRect->y) > m_disparity)continue;
		if(abs(pObj->m_boundBox.width - pRect->width) > m_disparity)continue;
		if(abs(pObj->m_boundBox.height - pRect->height) > m_disparity)continue;

		//The region is already under recognizing
		pObj->m_frameID = frameID;
//		pObj->m_status = OBJ_ADDED;

		//Update the Rect
		pObj->m_boundBox = *pRect;
		pObj->m_vContours = *pContour;

		return pObj;
	}

	if(iVacant < m_numObj)
	{
		//Change in status comes to the last
		pObj = &m_pObjects[iVacant];
		pObj->m_frameID = frameID;
		pObj->m_boundBox = *pRect;
		pObj->m_Mat = Mat(pRect->width,pRect->height,pMat->type());
		pMat->colRange(pRect->tl().x,pRect->br().x).rowRange(pRect->tl().y,pRect->br().y).copyTo(pObj->m_Mat);
		if(pContour)
		{
			pObj->m_vContours = *pContour;
		}
		pObj->m_status = OBJ_ADDED;

		return pObj;
	}


	return NULL;
}

} /* namespace kai */
