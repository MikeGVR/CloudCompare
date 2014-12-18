//##########################################################################
//#                                                                        #
//#                            CLOUDCOMPARE                                #
//#                                                                        #
//#  This program is free software; you can redistribute it and/or modify  #
//#  it under the terms of the GNU General Public License as published by  #
//#  the Free Software Foundation; version 2 of the License.               #
//#                                                                        #
//#  This program is distributed in the hope that it will be useful,       #
//#  but WITHOUT ANY WARRANTY; without even the implied warranty of        #
//#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         #
//#  GNU General Public License for more details.                          #
//#                                                                        #
//#          COPYRIGHT: EDF R&D / TELECOM ParisTech (ENST-TSI)             #
//#                                                                        #
//##########################################################################

#include "ccRasterizeTool.h"

//Local
#include "ccBoundingBoxEditorDlg.h"
#include "ccPersistentSettings.h"
#include "ccCommon.h"
#include "mainwindow.h"
#include "ccIsolines.h"

//qCC_db
#include <ccGenericPointCloud.h>
#include <ccPointCloud.h>
#include <ccScalarField.h>
#include <ccProgressDialog.h>
#include <ccPolyline.h>

//qCC_gl
#include <ccGLWindow.h>

//CCLib
#include <Delaunay2dMesh.h>

//Qt
#include <QSettings>
#include <QPushButton>
#include <QMessageBox>
#include <QImageWriter>
#include <QFileDialog>

//System
#include <assert.h>
#include <string.h>

ccRasterizeTool::ccRasterizeTool(ccGenericPointCloud* cloud, QWidget* parent/*=0*/)
	: QDialog(parent)
	, Ui::RasterizeToolDialog()
	, m_bbEditorDlg(0)
	, m_cloud(cloud)
	, m_window(0)
	, m_rasterCloud(0)
{
	setupUi(this);

	setWindowFlags(Qt::Tool/*Qt::Dialog | Qt::WindowStaysOnTopHint*/);

#ifndef CC_GDAL_SUPPORT
	generateRasterPushButton->setDisabled(true);
	generateRasterPushButton->setChecked(false);
#endif

	connect(buttonBox,					SIGNAL(accepted()),					this,	SLOT(saveSettings()));
	connect(gridStepDoubleSpinBox,		SIGNAL(valueChanged(double)),		this,	SLOT(updateGridInfo()));
	connect(gridStepDoubleSpinBox,		SIGNAL(valueChanged(double)),		this,	SLOT(gridOptionChanged()));
	connect(emptyValueDoubleSpinBox,	SIGNAL(valueChanged(double)),		this,	SLOT(gridOptionChanged()));
	connect(resampleCloudCheckBox,		SIGNAL(toggled(bool)),				this,	SLOT(gridOptionChanged()));
	connect(dimensionComboBox,			SIGNAL(currentIndexChanged(int)),	this,	SLOT(projectionDirChanged(int)));
	connect(heightProjectionComboBox,	SIGNAL(currentIndexChanged(int)),	this,	SLOT(projectionTypeChanged(int)));
	connect(fillEmptyCellsComboBox,		SIGNAL(currentIndexChanged(int)),	this,	SLOT(fillEmptyCellStrategyChanged(int)));
	connect(updateGridPushButton,		SIGNAL(clicked()),					this,	SLOT(updateGridAndDisplay()));
	connect(generateCloudPushButton,	SIGNAL(clicked()),					this,	SLOT(generateCloud()));
	connect(generateImagePushButton,	SIGNAL(clicked()),					this,	SLOT(generateImage()));
	connect(generateRasterPushButton,	SIGNAL(clicked()),					this,	SLOT(generateRaster()));
	connect(generateASCIIPushButton,	SIGNAL(clicked()),					this,	SLOT(generateASCIIMatrix()));
	connect(generateContoursPushButton,	SIGNAL(clicked()),					this,	SLOT(generateContours()));
	connect(exportContoursPushButton,	SIGNAL(clicked()),					this,	SLOT(exportContourLines()));
	connect(clearContoursPushButton,	SIGNAL(clicked()),					this,	SLOT(removeContourLines()));

	//custom bbox editor
	ccBBox gridBBox = m_cloud ? m_cloud->getMyOwnBB() : ccBBox(); 
	if (gridBBox.isValid())
	{
		m_bbEditorDlg = new ccBoundingBoxEditorDlg(this);
		m_bbEditorDlg->setBaseBBox(gridBBox,false);
		connect(editGridToolButton, SIGNAL(clicked()), this, SLOT(showGridBoxEditor()));
	}
	else
	{
		editGridToolButton->setEnabled(false);
	}

	if (m_cloud)
	{
		cloudNameLabel->setText(m_cloud->getName());
		pointCountLabel->setText(QString::number(m_cloud->size()));
		interpolateSFFrame->setEnabled(cloud->hasScalarFields());

		//add window
		{
			m_window = new ccGLWindow(this);
			ccGui::ParamStruct params = m_window->getDisplayParameters();
			//black (text) & white (background) display by default
			memcpy(params.backgroundCol,ccColor::white,3*sizeof(unsigned char));
			memcpy(params.textDefaultCol,ccColor::black,3*sizeof(unsigned char));
			params.drawBackgroundGradient = false;
			params.decimateMeshOnMove = false;
			params.displayCross = false;
			params.colorScaleUseShader = false;
			m_window->setDisplayParameters(params,true);
			m_window->setPerspectiveState(false,true);
			m_window->setInteractionMode(ccGLWindow::PAN_ONLY);
			m_window->displayOverlayEntities(true);
			m_window->setPickingMode(ccGLWindow::NO_PICKING);
			//add window to the right side layout
			mapFrame->setLayout(new QHBoxLayout());
			mapFrame->layout()->addWidget(m_window);
		}
	}

	loadSettings();

	updateGridInfo();

	gridIsUpToDate(false);
}

ccRasterizeTool::~ccRasterizeTool()
{
	if (m_rasterCloud)
	{
		if (m_window)
			m_window->removeFromOwnDB(m_rasterCloud);
		delete m_rasterCloud;
		m_rasterCloud = 0;
	}

	removeContourLines();

	m_grid.clear();
}

void ccRasterizeTool::removeContourLines()
{
	while (!m_contourLines.empty())
	{
		ccPolyline* poly = m_contourLines.back();
		if (m_window)
			m_window->removeFromOwnDB(poly);
		delete poly;
		m_contourLines.pop_back();
	}

	exportContoursPushButton->setEnabled(false);
	clearContoursPushButton->setEnabled(false);

	if (m_window)
		m_window->redraw();
}

void ccRasterizeTool::showGridBoxEditor()
{
	if (m_bbEditorDlg)
	{
		unsigned char projDim = getProjectionDimension();
		assert(projDim < 3);
		m_bbEditorDlg->set2DMode(true,projDim);
		if (m_bbEditorDlg->exec())
		{
			updateGridInfo();
			gridIsUpToDate(false);
		}
	}
}

void ccRasterizeTool::updateGridInfo()
{
	//vertical dimension
	const unsigned char Z = getProjectionDimension();
	assert(Z >= 0 && Z <= 2);
	const unsigned char X = Z == 2 ? 0 : Z +1;
	const unsigned char Y = X == 2 ? 0 : X +1;

	//cloud bounding-box --> grid size
	ccBBox box = getCustomBBox();
	if (box.isValid())
	{
		double gridStep = getGridStep();
		assert(gridStep != 0);

		CCVector3d boxDiag(	static_cast<double>(box.maxCorner().x) - static_cast<double>(box.minCorner().x),
							static_cast<double>(box.maxCorner().y) - static_cast<double>(box.minCorner().y),
							static_cast<double>(box.maxCorner().z) - static_cast<double>(box.minCorner().z) );

		unsigned gridWidth  = static_cast<unsigned>(ceil(boxDiag.u[X] / gridStep));
		unsigned gridHeight = static_cast<unsigned>(ceil(boxDiag.u[Y] / gridStep));

		gridWidthLabel->setText(QString("%1 x %2").arg(gridWidth).arg(gridHeight));
		gridHeightRangeLabel->setText(QString("%1 (%2 - %3)").arg(boxDiag.u[Z]).arg(box.minCorner().u[Z]).arg(box.maxCorner().u[Z]));

		contourStartDoubleSpinBox->setValue(box.minCorner().u[Z]);
		contourStepDoubleSpinBox->setValue(boxDiag.u[Z] / 10.0);
	}
	else
	{
		gridWidthLabel->setText("invalid grid box");
	}
}

ccBBox ccRasterizeTool::getCustomBBox() const
{
	return (m_bbEditorDlg ? m_bbEditorDlg->getBox() : ccBBox());
}

double ccRasterizeTool::getGridStep() const
{
	return gridStepDoubleSpinBox->value();
}

bool ccRasterizeTool::generateCountSF() const
{
	return generateCountSFcheckBox->isChecked();
}

bool ccRasterizeTool::resampleOriginalCloud() const
{
	return resampleCloudCheckBox->isEnabled() && resampleCloudCheckBox->isChecked();
}

unsigned char ccRasterizeTool::getProjectionDimension() const
{
	int dim = dimensionComboBox->currentIndex();
	assert(dim >= 0 && dim < 3);

	return static_cast<unsigned char>(dim);
}

void ccRasterizeTool::projectionTypeChanged(int index)
{
	//we can't use the 'resample origin cloud' option with 'average height' projection
	resampleCloudCheckBox->setEnabled(index != PROJ_AVERAGE_VALUE);
	gridIsUpToDate(false);
}

void ccRasterizeTool::projectionDirChanged(int dir)
{
	updateGridInfo();
	gridIsUpToDate(false);
}

void ccRasterizeTool::fillEmptyCellStrategyChanged(int)
{
	EmptyCellFillOption fillEmptyCellsStrategy = getFillEmptyCellsStrategy();

	emptyValueDoubleSpinBox->setEnabled(	fillEmptyCellsStrategy == FILL_CUSTOM_HEIGHT
										||	fillEmptyCellsStrategy == INTERPOLATE );
	gridIsUpToDate(false);
}

void ccRasterizeTool::gridOptionChanged()
{
	gridIsUpToDate(false);
}

double ccRasterizeTool::getCustomHeightForEmptyCells() const
{
	return emptyValueDoubleSpinBox->value();
}

ccRasterizeTool::ProjectionType ccRasterizeTool::getTypeOfProjection() const
{
	switch (heightProjectionComboBox->currentIndex())
	{
	case 0:
		return PROJ_MINIMUM_VALUE;
	case 1:
		return PROJ_AVERAGE_VALUE;
	case 2:
		return PROJ_MAXIMUM_VALUE;
	default:
		//shouldn't be possible for this option!
		assert(false);
	}

	return INVALID_PROJECTION_TYPE;
}

ccRasterizeTool::ProjectionType ccRasterizeTool::getTypeOfSFInterpolation() const
{
	if (!interpolateSFFrame->isEnabled() || !interpolateSFCheckBox->isChecked())
		return INVALID_PROJECTION_TYPE; //means that we don't want to keep SF values

	switch (scalarFieldProjection->currentIndex())
	{
	case 0:
		return PROJ_MINIMUM_VALUE;
	case 1:
		return PROJ_AVERAGE_VALUE;
	case 2:
		return PROJ_MAXIMUM_VALUE;
	default:
		//shouldn't be possible for this option!
		assert(false);
	}

	return INVALID_PROJECTION_TYPE;
}

ccRasterizeTool::EmptyCellFillOption ccRasterizeTool::getFillEmptyCellsStrategy() const
{
	switch (fillEmptyCellsComboBox->currentIndex())
	{
	case 0:
		return LEAVE_EMPTY;
	case 1:
		return FILL_MINIMUM_HEIGHT;
	case 2:
		return FILL_AVERAGE_HEIGHT;
	case 3:
		return FILL_MAXIMUM_HEIGHT;
	case 4:
		return FILL_CUSTOM_HEIGHT;
	case 5:
		return INTERPOLATE;
	default:
		//shouldn't be possible for this option!
		assert(false);
	}

	return LEAVE_EMPTY;
}

ccRasterizeTool::EmptyCellFillOption ccRasterizeTool::getFillEmptyCellsStrategy(double& emptyCellsHeight,
																				double& minHeight,
																				double& maxHeight) const
{
	EmptyCellFillOption fillEmptyCellsStrategy = getFillEmptyCellsStrategy();

	emptyCellsHeight = 0.0;
	minHeight = m_grid.minHeight;
	maxHeight = m_grid.maxHeight;
	
	switch (fillEmptyCellsStrategy)
	{
	case LEAVE_EMPTY:
		//nothing to do
		break;
	case FILL_MINIMUM_HEIGHT:
		emptyCellsHeight = m_grid.minHeight;
		break;
	case FILL_MAXIMUM_HEIGHT:
		emptyCellsHeight = m_grid.maxHeight;
		break;
	case FILL_CUSTOM_HEIGHT:
	case INTERPOLATE:
		{
			double customEmptyCellsHeight = getCustomHeightForEmptyCells();
			//update min and max height by the way (only if there are empty cells ;)
			if (m_grid.nonEmptyCells != m_grid.width * m_grid.height)
			{
				if (customEmptyCellsHeight <= m_grid.minHeight)
					minHeight = customEmptyCellsHeight;
				else if (customEmptyCellsHeight >= m_grid.maxHeight)
					maxHeight = customEmptyCellsHeight;
				emptyCellsHeight = customEmptyCellsHeight;
			}
		}
		break;
	case FILL_AVERAGE_HEIGHT:
		//'average height' is a kind of 'custom height' so we can fall back to this mode!
		fillEmptyCellsStrategy = FILL_CUSTOM_HEIGHT;
		emptyCellsHeight = m_grid.meanHeight;
		break;
	default:
		assert(false);
	}

	return fillEmptyCellsStrategy;
}

void ccRasterizeTool::loadSettings()
{
	QSettings settings;
	settings.beginGroup(ccPS::HeightGridGeneration());
	int projType		= settings.value("ProjectionType",heightProjectionComboBox->currentIndex()).toInt();
	int projDim			= settings.value("ProjectionDim",dimensionComboBox->currentIndex()).toInt();
	bool sfProj			= settings.value("SfProjEnabled",interpolateSFCheckBox->isChecked()).toBool();
	int sfProjStrategy	= settings.value("SfProjStrategy",scalarFieldProjection->currentIndex()).toInt();
	int fillStrategy	= settings.value("FillStrategy",fillEmptyCellsComboBox->currentIndex()).toInt();
	double step			= settings.value("GridStep",gridStepDoubleSpinBox->value()).toDouble();
	double emptyHeight	= settings.value("EmptyCellsHeight",emptyValueDoubleSpinBox->value()).toDouble();
	bool genCountSF		= settings.value("GenerateCountSF",generateCountSFcheckBox->isChecked()).toBool();
	bool resampleCloud	= settings.value("ResampleOrigCloud",resampleCloudCheckBox->isChecked()).toBool();
	int minVertexCount	= settings.value("MinVertexCount",minVertexCountSpinBox->value()).toInt();
	bool ignoreBorders	= settings.value("IgnoreBorders",ignoreContourBordersCheckBox->isChecked()).toBool();
	settings.endGroup();

	gridStepDoubleSpinBox->setValue(step);
	heightProjectionComboBox->setCurrentIndex(projType);
	fillEmptyCellsComboBox->setCurrentIndex(fillStrategy);
	emptyValueDoubleSpinBox->setValue(emptyHeight);
	dimensionComboBox->setCurrentIndex(projDim);
	interpolateSFCheckBox->setChecked(sfProj);
	scalarFieldProjection->setCurrentIndex(sfProjStrategy);
	generateCountSFcheckBox->setChecked(genCountSF);
	resampleCloudCheckBox->setChecked(resampleCloud);
	minVertexCountSpinBox->setValue(minVertexCount);
	ignoreContourBordersCheckBox->setChecked(ignoreBorders);
}

void ccRasterizeTool::saveSettings()
{
	QSettings settings;
	settings.beginGroup(ccPS::HeightGridGeneration());
	settings.setValue("ProjectionType",heightProjectionComboBox->currentIndex());
	settings.setValue("ProjectionDim",dimensionComboBox->currentIndex());
	settings.setValue("SfProjEnabled",interpolateSFCheckBox->isChecked());
	settings.setValue("SfProjStrategy",scalarFieldProjection->currentIndex());
	settings.setValue("FillStrategy",fillEmptyCellsComboBox->currentIndex());
	settings.setValue("GridStep",gridStepDoubleSpinBox->value());
	settings.setValue("EmptyCellsHeight",emptyValueDoubleSpinBox->value());
	settings.setValue("GenerateCountSF",generateCountSFcheckBox->isChecked());
	settings.setValue("ResampleOrigCloud",resampleCloudCheckBox->isChecked());
	settings.setValue("MinVertexCount",minVertexCountSpinBox->value());
	settings.setValue("IgnoreBorders",ignoreContourBordersCheckBox->isChecked());
	settings.endGroup();
}

void ccRasterizeTool::update2DDisplayZoom(ccBBox& box)
{
	if (!m_window || !m_grid.isValid())
		return;

	//equivalent to 'ccGLWindow::updateConstellationCenterAndZoom' but we take aspect ratio into account

	//we compute the pixel size (in world coordinates)
	{
		ccViewportParameters params = m_window->getViewportParameters();

		double realGridWidth  = m_grid.width  * m_grid.gridStep;
		double realGridHeight = m_grid.height * m_grid.gridStep;

		static const int screnMargin = 20;
		int screenWidth  = std::max(1,m_window->width()  - 2*screnMargin);
		int screenHeight = std::max(1,m_window->height() - 2*screnMargin);

		int pointSize = 1;
		if (	static_cast<int>(m_grid.width)  < screenWidth
			&&	static_cast<int>(m_grid.height) < screenHeight)
		{
			int vPointSize = static_cast<int>(ceil(static_cast<float>(screenWidth) /m_grid.width));
			int hPointSize = static_cast<int>(ceil(static_cast<float>(screenHeight)/m_grid.height));
			pointSize = std::min(vPointSize,hPointSize);

			//if the grid is too small (i.e. necessary point size > 10)
			if (pointSize > 10)
			{
				pointSize = 10;
				screenWidth  = m_grid.width  * pointSize;
				screenHeight = m_grid.height * pointSize;
			}
		}

		params.pixelSize = static_cast<float>( std::max( realGridWidth/screenWidth, realGridHeight/screenHeight ) );
		params.zoom = 1.0f;

		m_window->setViewportParameters(params);
		m_window->setPointSize(pointSize);
	}
	
	//we set the pivot point on the box center
	CCVector3 P = box.getCenter();
	m_window->setPivotPoint(CCVector3d::fromArray(P.u));
	m_window->setCameraPos(CCVector3d::fromArray(P.u));

	m_window->invalidateViewport();
	m_window->invalidateVisualization();
	m_window->redraw();
}

void ccRasterizeTool::gridIsUpToDate(bool state)
{
	if (state)
	{
		//standard button
		updateGridPushButton->setStyleSheet(QString());
	}
	else
	{
		//red button
		updateGridPushButton->setStyleSheet("color: white; background-color:red;");
	}
	updateGridPushButton->setDisabled(state);

	tabWidget->setEnabled(state);
}

void ccRasterizeTool::RasterGrid::clear()
{
	//reset
	width = height = 0;

	//properly clean memory
	for (size_t i=0; i<data.size(); ++i)
	{
		if (data[i])
			delete[] data[i];
	}
	data.clear();

	for (size_t j=0; j<scalarFields.size(); ++j)
	{
		if (scalarFields[j])
			delete[] scalarFields[j];
	}
	scalarFields.clear();
}

void ccRasterizeTool::RasterGrid::reset()
{
	//reset values
	for (size_t j=0; j<data.size(); ++j)
	{
		RasterCell* cell = data[j];
		for (unsigned i=0; i<width; ++i, ++cell)
		{
			*cell = RasterCell();
		}
	}

	//not necessary
	//for (size_t j=0; j<scalarFields.size(); ++j)
	//{
	//	if (scalarFields[j])
	//		memset(scalarFields[j],0,sizeof(double)*width*height);
	//}

	minHeight = maxHeight = meanHeight = 0;
	nonEmptyCells = 0;
}

bool ccRasterizeTool::RasterGrid::init(unsigned w, unsigned h)
{
	setValid(false);

	if (w == width && h == height)
	{
		//simply reset values
		reset();
		return true;
	}

	clear();

	try
	{
		data.resize(h,0);
		for (unsigned i=0; i<h; ++i)
		{
			data[i] = new RasterCell[w];
			if (!data[i])
			{
				//not enough memory
				clear();
				return false;
			}
		}
	}
	catch(std::bad_alloc)
	{
		//not enough memory
		return false;
	}

	width = w;
	height = h;

	return true;
}

ccPointCloud* ccRasterizeTool::convertGridToCloud(bool generateCountSF, bool interpolateSF) const
{
	if (!m_cloud || !m_grid.isValid())
		return 0;

	//default values
	double emptyCellsHeight = 0;
	double minHeight = m_grid.minHeight;
	double maxHeight = m_grid.maxHeight;
	//get real values
	EmptyCellFillOption fillEmptyCellsStrategy = getFillEmptyCellsStrategy(	emptyCellsHeight,
																			minHeight,
																			maxHeight);

	unsigned pointsCount = (fillEmptyCellsStrategy != LEAVE_EMPTY ? m_grid.width * m_grid.height : m_grid.nonEmptyCells);
	if (pointsCount == 0)
	{
		ccLog::Warning("[Rasterize] Empty grid!");
		return 0;
	}

	ccPointCloud* cloudGrid = 0;
	bool resampleInputCloud = resampleOriginalCloud();
	if (resampleInputCloud)
	{
		CCLib::ReferenceCloud refCloud(m_cloud);
		if (refCloud.reserve(m_grid.nonEmptyCells))
		{
			for (unsigned j=0; j<m_grid.height; ++j)
			{
				const RasterCell* aCell = m_grid.data[j];
				for (unsigned i=0; i<m_grid.width; ++i,++aCell)
				{
					if (aCell->nbPoints) //non empty cell
					{
						refCloud.addPointIndex(aCell->pointIndex);
					}
				}
			}

			assert(refCloud.size() != 0);
			cloudGrid = m_cloud->isA(CC_TYPES::POINT_CLOUD) ? static_cast<ccPointCloud*>(m_cloud)->partialClone(&refCloud) : ccPointCloud::From(&refCloud);

			//even if we have already resampled the original cloud we may have to create new points and/or scalar fields
			//if (!interpolateSF && fillEmptyCellsStrategy == LEAVE_EMPTY)
			//	return cloudGrid;
		}
		else
		{
			ccLog::Warning("[Rasterize] Not enough memory!");
			return 0;
		}
	}
	else
	{
		cloudGrid = new ccPointCloud("grid");
	}
	assert(cloudGrid);
	
	//per-point height SF
	CCLib::ScalarField* heightSF = 0;
	int heightSFIdx = -1;
	{
		heightSFIdx = cloudGrid->addScalarField(CC_HEIGHT_GRID_FIELD_NAME);
		if (heightSFIdx < 0)
		{
			ccLog::Warning("[Rasterize] Couldn't allocate a new scalar field for storing height grid values! Try to free some memory ...");
		}
		else
		{
			heightSF = cloudGrid->getScalarField(heightSFIdx);
			assert(heightSF);
		}
	}

	//shall we save per-cell population as well?
	CCLib::ScalarField* countSF = 0;
	int countSFIdx = -1;
	if (generateCountSF)
	{
		countSFIdx = cloudGrid->addScalarField("Per-cell population");
		if (countSFIdx < 0)
		{
			ccLog::Warning("[Rasterize] Couldn't allocate a new scalar field for storing per-cell population count! Try to free some memory ...");
		}
		else
		{
			countSF = cloudGrid->getScalarField(countSFIdx);
			assert(countSF);
		}
	}

	//the resampled cloud already contains the points corresponding to 'filled' cells so we will only
	//need to add the empty ones (if requested)
	if (!(resampleInputCloud && fillEmptyCellsStrategy == LEAVE_EMPTY) && !cloudGrid->reserve(pointsCount))
	{
		ccLog::Warning("[Rasterize] Not enough memory!");
		delete cloudGrid;
		return 0;
	}

	//vertical dimension
	const unsigned char Z = getProjectionDimension();
	assert(Z >= 0 && Z <= 2);
	const unsigned char X = Z == 2 ? 0 : Z +1;
	const unsigned char Y = X == 2 ? 0 : X +1;

	//cloud bounding-box
	ccBBox box = getCustomBBox();
	assert(box.isValid());

	//we work with doubles as grid step can be much smaller than the cloud coordinates!
	double Py = box.minCorner().u[Y];

	//as the 'non empty cells points' are already in the cloud
	//we must take care of where we put the scalar fields values!
	unsigned nonEmptyCellIndex = 0;

	for (unsigned j=0; j<m_grid.height; ++j)
	{
		const RasterCell* aCell = m_grid.data[j];
		double Px = box.minCorner().u[X];
		for (unsigned i=0; i<m_grid.width; ++i,++aCell)
		{
			if (aCell->nbPoints) //non empty cell
			{
				//if we haven't resampled the original cloud, we must add the point
				//corresponding to this non-empty cell
				if (!resampleInputCloud)
				{
					double Pz = static_cast<double>(aCell->height);

					CCVector3 Pf(	static_cast<PointCoordinateType>(Px),
									static_cast<PointCoordinateType>(Py),
									static_cast<PointCoordinateType>(Pz) );

					cloudGrid->addPoint(Pf);
				}

				//if a SF is available, we set the point height equal to this cell's height
				assert(nonEmptyCellIndex < m_cloud->size());
				if (heightSF)
				{
					ScalarType h = static_cast<ScalarType>(aCell->height);
					if (resampleInputCloud)
						heightSF->setValue(nonEmptyCellIndex,h);
					else
						heightSF->addElement(h);
				}
				//same thing for the 'per-cell population' SF
				if (countSF)
				{
					ScalarType pop = static_cast<ScalarType>(aCell->nbPoints);
					if (resampleInputCloud)
						countSF->setValue(nonEmptyCellIndex,pop);
					else
						countSF->addElement(pop);
				}
				++nonEmptyCellIndex;
			}
			else if (fillEmptyCellsStrategy != LEAVE_EMPTY) //empty cell
			{
				//even if we have resampled the original cloud, we must add the point
				//corresponding to this empty cell
				{
					CCVector3 Pf(	static_cast<PointCoordinateType>(Px),
									static_cast<PointCoordinateType>(Py),
									static_cast<PointCoordinateType>(emptyCellsHeight) );
					cloudGrid->addPoint(Pf);
				}

				//if a SF is available, we set the point height to the default height
				if (heightSF)
				{
					ScalarType s = static_cast<ScalarType>(emptyCellsHeight);
					heightSF->addElement(s);
				}
				if (countSF)
				{
					countSF->addElement(NAN_VALUE);
				}
			}

			Px += m_grid.gridStep;
		}

		Py += m_grid.gridStep;
	}

	if (heightSF)
	{
		heightSF->computeMinAndMax();
		cloudGrid->setCurrentDisplayedScalarField(heightSFIdx);
	}
	if (countSF)
	{
		countSF->computeMinAndMax();
		if (!heightSF)
			cloudGrid->setCurrentDisplayedScalarField(countSFIdx);
	}
	cloudGrid->showSF(heightSF || countSF);

	//take care of former scalar fields
	if (!resampleInputCloud)
	{
		if (interpolateSF && m_cloud->isA(CC_TYPES::POINT_CLOUD))
		{
			ccPointCloud* pc = static_cast<ccPointCloud*>(m_cloud);
			for (size_t k=0; k<m_grid.scalarFields.size(); ++k)
			{
				double* _sfGrid = m_grid.scalarFields[k];
				if (_sfGrid) //valid SF grid
				{
					//the corresponding SF should exist on the input cloud
					CCLib::ScalarField* formerSf = pc->getScalarField(static_cast<int>(k));
					assert(formerSf);

					//we try to create an equivalent SF on the output grid
					int sfIdx = cloudGrid->addScalarField(formerSf->getName());
					if (sfIdx < 0) //if we aren't lucky, the input cloud already had a SF with CC_HEIGHT_GRID_FIELD_NAME as name
						sfIdx = cloudGrid->addScalarField(qPrintable(QString(formerSf->getName()).append(".old")));

					if (sfIdx < 0)
					{
						ccLog::Warning("[Rasterize] Couldn't allocate a new scalar field for storing SF '%s' values! Try to free some memory ...",formerSf->getName());
					}
					else
					{
						CCLib::ScalarField* sf = cloudGrid->getScalarField(sfIdx);
						assert(sf);
						//set sf values
						unsigned n = 0;
						const ScalarType emptyCellSFValue = CCLib::ScalarField::NaN();
						for (unsigned j=0; j<m_grid.height; ++j)
						{
							const RasterCell* aCell = m_grid.data[j];
							for (unsigned i=0; i<m_grid.width; ++i, ++_sfGrid, ++aCell)
							{
								if (aCell->nbPoints)
								{
									ScalarType s = static_cast<ScalarType>(*_sfGrid);
									sf->setValue(n++,s);
								}
								else if (fillEmptyCellsStrategy != LEAVE_EMPTY)
								{
									sf->setValue(n++,emptyCellSFValue);
								}
							}
						}
						sf->computeMinAndMax();
						assert(sf->currentSize() == pointsCount);
					}
				}
			}
		}
	}
	else
	{
		for (size_t k=0; k<cloudGrid->getNumberOfScalarFields(); ++k)
		{
			CCLib::ScalarField* sf = cloudGrid->getScalarField(static_cast<int>(k));
			sf->resize(cloudGrid->size(),true,NAN_VALUE);
		}
	}

	cloudGrid->setName(m_cloud->getName() + QString(".raster(%1)").arg(m_grid.gridStep));

	//don't forget original shift
	cloudGrid->setGlobalShift(m_cloud->getGlobalShift());
	cloudGrid->setGlobalScale(m_cloud->getGlobalScale());

	return cloudGrid;
}

void ccRasterizeTool::updateGridAndDisplay()
{
	bool success = updateGrid(getTypeOfSFInterpolation() != INVALID_PROJECTION_TYPE);

	if (success && m_window)
	{
		//convert grid to point cloud
		if (m_rasterCloud)
		{
			m_window->removeFromOwnDB(m_rasterCloud);
			delete m_rasterCloud;
			m_rasterCloud = 0;
		}

		m_rasterCloud = convertGridToCloud(false,false);

		if (m_rasterCloud)
		{
			m_window->addToOwnDB(m_rasterCloud);
			ccBBox box = m_rasterCloud->getMyOwnBB();
			update2DDisplayZoom(box);
		}
		else
		{
			m_window->redraw();
		}
	}

	gridIsUpToDate(success);
}

bool ccRasterizeTool::updateGrid(bool interpolateSF/*=false*/)
{
	if (!m_cloud)
	{
		assert(false);
		return false;
	}

	//main parameters
	ProjectionType projectionType = getTypeOfProjection();
	ProjectionType sfInterpolation = getTypeOfSFInterpolation();
	EmptyCellFillOption fillEmptyCellsStrategy = getFillEmptyCellsStrategy();

	//vertical dimension
	const unsigned char Z = getProjectionDimension();
	assert(Z >= 0 && Z <= 2);
	const unsigned char X = Z == 2 ? 0 : Z +1;
	const unsigned char Y = X == 2 ? 0 : X +1;

	//cloud bounding-box --> grid size
	ccBBox box = getCustomBBox();
	if (!box.isValid())
		return false;

	double gridStep = getGridStep();
	assert(gridStep != 0);

	CCVector3d boxDiag(	static_cast<double>(box.maxCorner().x) - static_cast<double>(box.minCorner().x),
						static_cast<double>(box.maxCorner().y) - static_cast<double>(box.minCorner().y),
						static_cast<double>(box.maxCorner().z) - static_cast<double>(box.minCorner().z) );

	if (boxDiag.u[X] <= 0 || boxDiag.u[Y] <= 0)
	{
		ccLog::Error("Invalid cloud bounding box!");
		return false;
	}

	unsigned gridWidth  = static_cast<unsigned>(ceil(boxDiag.u[X] / gridStep));
	unsigned gridHeight = static_cast<unsigned>(ceil(boxDiag.u[Y] / gridStep));

	//grid size
	unsigned gridTotalSize = gridWidth * gridHeight;
	if (gridTotalSize == 1)
	{
		if (QMessageBox::question(0,"Unexpected grid size","The generated grid will only have 1 cell! Do you want to proceed anyway?",QMessageBox::Yes,QMessageBox::No) == QMessageBox::No)
			return false;
	}
	else if (gridTotalSize > 10000000)
	{
		if (QMessageBox::question(0,"Big grid size","The generated grid will have more than 10.000.000 cells! Do you want to proceed anyway?",QMessageBox::Yes,QMessageBox::No) == QMessageBox::No)
			return false;
	}

	removeContourLines();

	//memory allocation
	if (!m_grid.init(gridWidth,gridHeight))
	{
		//not enough memory
		ccLog::Error("Not enough memory");
		return false;
	}
	m_grid.gridStep = gridStep;

	//do we need to interpolate scalar fields?
	ccPointCloud* pc = m_cloud->isA(CC_TYPES::POINT_CLOUD) ? static_cast<ccPointCloud*>(m_cloud) : 0;
	interpolateSF &= (pc && pc->hasScalarFields());
	if (interpolateSF)
	{
		unsigned sfCount = pc->getNumberOfScalarFields();

		bool memoryError = false;
		try
		{
			m_grid.scalarFields.resize(sfCount,0);
		}
		catch(std::bad_alloc)
		{
			//not enough memory
			memoryError = true;
		}

		for (unsigned i=0; i<sfCount; ++i)
		{
			m_grid.scalarFields[i] = new double[gridTotalSize];
			if (!m_grid.scalarFields[i])
			{
				//not enough memory
				memoryError = true;
				break;
			}
		}

		if (memoryError)
			ccLog::Warning(QString("[Rasterize] Failed to allocate memory for scalar fields!"));
	}

	//filling the grid
	unsigned pointCount = m_cloud->size();

	CCVector3d minGridCorner = CCVector3d::fromArray(box.minCorner().u);
	double gridMaxX = gridStep * m_grid.width;
	double gridMaxY = gridStep * m_grid.height;

	ccProgressDialog pDlg(true,this);
	pDlg.setMethodTitle("Grid generation");
	pDlg.setInfo(qPrintable(QString("Points: %1\nCells: %2 x %3").arg(pointCount).arg(m_grid.width).arg(m_grid.height)));
	pDlg.start();
	pDlg.show();
	QApplication::processEvents();
	CCLib::NormalizedProgress nProgress(&pDlg,pointCount);

	for (unsigned n=0; n<pointCount; ++n)
	{
		const CCVector3* P = m_cloud->getPoint(n);

		CCVector3d relativePos = CCVector3d::fromArray(P->u) - minGridCorner;
		int i = static_cast<int>(relativePos.u[X]/gridStep);
		int j = static_cast<int>(relativePos.u[Y]/gridStep);

		//specific case: if we fall exactly on the max corner of the grid box
		if (i == static_cast<int>(m_grid.width) && relativePos.u[X] == gridMaxX)
			--i;
		if (j == static_cast<int>(m_grid.height) && relativePos.u[Y] == gridMaxY)
			--j;

		//we skip points outside the box!
		if (	i < 0 || i >= static_cast<int>(m_grid.width)
			||	j < 0 || j >= static_cast<int>(m_grid.height) )
			continue;

		assert(i >= 0 && j >= 0);

		RasterCell* aCell = m_grid.data[j]+i;
		unsigned& pointsInCell = aCell->nbPoints;
		if (pointsInCell)
		{
			switch (projectionType)
			{
			case PROJ_MINIMUM_VALUE:
				// Set the minimum height
				if (P->u[Z] < aCell->height)
				{
					aCell->height = P->u[Z];
					aCell->pointIndex = n;
				}
				break;
			case PROJ_MAXIMUM_VALUE:
				// Set the maximum height
				if (P->u[Z] > aCell->height)
				{
					aCell->height = P->u[Z];
					aCell->pointIndex = n;
				}
				break;
			case PROJ_AVERAGE_VALUE:
				// Sum the points heights
				aCell->height += P->u[Z];
				break;
			default:
				assert(false);
				break;
			}
		}
		else
		{
			//for the first point, we simply have to store its height (in any case)
			aCell->height = P->u[Z];
			aCell->pointIndex = n;
		}

		//scalar fields
		if (interpolateSF)
		{
			assert(sfInterpolation != INVALID_PROJECTION_TYPE);
			int pos = j*static_cast<int>(m_grid.width)+i; //pos in 2D SF grid(s)
			assert(pos < static_cast<int>(gridTotalSize));
			for (size_t k=0; k<m_grid.scalarFields.size(); ++k)
			{
				if (m_grid.scalarFields[k])
				{
					CCLib::ScalarField* sf = pc->getScalarField(static_cast<unsigned>(k));
					assert(sf);
					ScalarType sfValue = sf->getValue(n);
					ScalarType formerValue = static_cast<ScalarType>(m_grid.scalarFields[k][pos]);

					if (pointsInCell && ccScalarField::ValidValue(formerValue))
					{
						if (ccScalarField::ValidValue(sfValue))
						{
							switch (sfInterpolation)
							{
							case PROJ_MINIMUM_VALUE:
								// keep the minimum value
								m_grid.scalarFields[k][pos] = std::min<double>(formerValue,sfValue);
								break;
							case PROJ_AVERAGE_VALUE:
								//we sum all values (we will divide them later)
								m_grid.scalarFields[k][pos] += sfValue;
								break;
							case PROJ_MAXIMUM_VALUE:
								// keep the maximum value
								m_grid.scalarFields[k][pos] = std::max<double>(formerValue,sfValue);
								break;
							default:
								break;
							}
						}
					}
					else
					{
						//for the first (vaild) point, we simply have to store its SF value (in any case)
						m_grid.scalarFields[k][pos] = sfValue;
					}
				}
			}
		}

		pointsInCell++;

		if (!nProgress.oneStep())
		{
			//process cancelled by user
			return false;
		}
	}

	//update grids for 'average' cases
	if (sfInterpolation == PROJ_AVERAGE_VALUE)
	{
		for (size_t k=0; k<m_grid.scalarFields.size(); ++k)
		{
			if (m_grid.scalarFields[k])
			{
				double* _gridSF = m_grid.scalarFields[k];
				for (unsigned j=0;j<m_grid.height;++j)
				{
					RasterCell* cell = m_grid.data[j];
					for (unsigned i=0; i<m_grid.width; ++i,++cell,++_gridSF)
					{
						if (cell->nbPoints)
						{
							ScalarType s = static_cast<ScalarType>(*_gridSF);
							if (ccScalarField::ValidValue(s)) //valid SF value
								*_gridSF /= static_cast<double>(cell->nbPoints);
						}
					}
				}
			}
		}
	}

	//we need to finish the average height computation
	if (projectionType == PROJ_AVERAGE_VALUE)
	{
		for (unsigned j=0; j<m_grid.height; ++j)
		{
			RasterCell* cell = m_grid.data[j];
			for (unsigned i=0; i<m_grid.width; ++i,++cell)
				if (cell->nbPoints > 1)
					cell->height /= static_cast<PointCoordinateType>(cell->nbPoints);
		}
	}
	
	//fill empty cells by interpolating nearest values
	if (fillEmptyCellsStrategy == INTERPOLATE)
	{
		//compute the number of non empty cells
		unsigned nonEmptyCells = 0;
		{
			for (unsigned i=0; i<m_grid.height; ++i)
				for (unsigned j=0; j<m_grid.width; ++j)
					if (m_grid.data[i][j].nbPoints) //non empty cell
						nonEmptyCells++;
		}

		std::vector<CCVector2> the2DPoints;
		if (nonEmptyCells > 2 && nonEmptyCells != m_grid.width * m_grid.height)
		{
			try
			{
				the2DPoints.resize(nonEmptyCells);
			}
			catch (...)
			{
				//out of memory
				ccLog::Warning("[Rasterize] Not enough memory to interpolate empty cells!");
			}
		}
		
		//fill 2D vector with non-empty cell indexes
		if (!the2DPoints.empty())
		{
			unsigned index = 0;
			for (unsigned j=0; j<m_grid.height; ++j)
			{
				const RasterCell* cell = m_grid.data[j];
				for (unsigned i=0; i<m_grid.width; ++i, ++cell)
					if (cell->nbPoints)
						the2DPoints[index++] = CCVector2(static_cast<PointCoordinateType>(i),static_cast<PointCoordinateType>(j));
			}
			assert(index == nonEmptyCells);

			//mesh the '2D' points
			CCLib::Delaunay2dMesh* dm = new CCLib::Delaunay2dMesh();
			char errorStr[1024];
			if (!dm->buildMesh(the2DPoints,0,errorStr))
			{
				ccLog::Warning(QString("[Rasterize] Empty cells interpolation failed: Triangle lib. said '%1'").arg(errorStr));
			}
			else
			{
				unsigned triNum = dm->size();
				//now we are going to 'project' all triangles on the grid
				dm->placeIteratorAtBegining();
				for (unsigned k=0; k<triNum; ++k)
				{
					const CCLib::TriangleSummitsIndexes* tsi = dm->getNextTriangleIndexes();
					//get the triangle bounding box (in grid coordinates)
					int P[3][2];
					int xMin=0,yMin=0,xMax=0,yMax=0;
					{
						for (unsigned j=0; j<3; ++j)
						{
							const CCVector2& P2D = the2DPoints[tsi->i[j]];
							P[j][0] = static_cast<int>(P2D.x);
							P[j][1] = static_cast<int>(P2D.y);
						}
						xMin = std::min(std::min(P[0][0],P[1][0]),P[2][0]);
						yMin = std::min(std::min(P[0][1],P[1][1]),P[2][1]);
						xMax = std::max(std::max(P[0][0],P[1][0]),P[2][0]);
						yMax = std::max(std::max(P[0][1],P[1][1]),P[2][1]);
					}
					//now scan the cells
					{
						//pre-computation for barycentric coordinates
						const double& valA = m_grid.data[ P[0][1] ][ P[0][0] ].height;
						const double& valB = m_grid.data[ P[1][1] ][ P[1][0] ].height;
						const double& valC = m_grid.data[ P[2][1] ][ P[2][0] ].height;

						int det = (P[1][1]-P[2][1])*(P[0][0]-P[2][0]) + (P[2][0]-P[1][0])*(P[0][1]-P[2][1]);

						for (int j=yMin; j<=yMax; ++j)
						{
							RasterCell* cell = m_grid.data[static_cast<unsigned>(j)];

							for (int i=xMin; i<=xMax; ++i)
							{
								//if the cell is empty
								if (!cell[i].nbPoints)
								{
									//we test if it's included or not in the current triangle
									//Point Inclusion in Polygon Test (inspired from W. Randolph Franklin - WRF)
									bool inside = false;
									for (int ti=0; ti<3; ++ti)
									{
										const int* P1 = P[ti];
										const int* P2 = P[(ti+1)%3];
										if ((P2[1] <= j &&j < P1[1]) || (P1[1] <= j && j < P2[1]))
										{
											int t = (i-P2[0])*(P1[1]-P2[1])-(P1[0]-P2[0])*(j-P2[1]);
											if (P1[1] < P2[1])
												t = -t;
											if (t < 0)
												inside = !inside;
										}
									}
									//can we interpolate?
									if (inside)
									{
										double l1 = static_cast<double>((P[1][1]-P[2][1])*(i-P[2][0])+(P[2][0]-P[1][0])*(j-P[2][1]))/det;
										double l2 = static_cast<double>((P[2][1]-P[0][1])*(i-P[2][0])+(P[0][0]-P[2][0])*(j-P[2][1]))/det;
										double l3 = 1.0-l1-l2;

										cell[i].nbPoints = 1;
										cell[i].height = l1 * valA + l2 * valB + l3 * valC;
									}
								}
							}
						}
					}
				}
			}

			delete dm;
			dm = 0;
		}
	}

	//computation of the average and extreme height values in the grid
	m_grid.minHeight = 0;
	m_grid.maxHeight = 0;
	m_grid.meanHeight = 0;
	//non empty cells count
	m_grid.nonEmptyCells = 0;
	{
		for (unsigned i=0; i<m_grid.height; ++i)
		{
			for (unsigned j=0; j<m_grid.width; ++j)
			{
				if (m_grid.data[i][j].nbPoints) //non empty cell
				{
					double h = m_grid.data[i][j].height;

					if (m_grid.nonEmptyCells)
					{
						if (h < m_grid.minHeight)
							m_grid.minHeight = h;
						else if (h > m_grid.maxHeight)
							m_grid.maxHeight = h;
						m_grid.meanHeight += h;
					}
					else
					{
						m_grid.meanHeight = m_grid.minHeight = m_grid.maxHeight = h;
					}
					m_grid.nonEmptyCells++;
				}
			}
		}
	}

	m_grid.setValid(true);

	return true;
}

void ccRasterizeTool::generateCloud() const
{
	//to avoid multiple clicks
	generateCloudPushButton->blockSignals(true);
	
	ccPointCloud* rasterCloud = convertGridToCloud(generateCountSF(),getTypeOfSFInterpolation() != INVALID_PROJECTION_TYPE);

	if (rasterCloud)
	{
		if (m_cloud->getParent())
			m_cloud->getParent()->addChild(rasterCloud);
		rasterCloud->setDisplay(m_cloud->getDisplay());
		
		if (m_cloud->isEnabled())
		{
			m_cloud->setEnabled(false);
			ccLog::Warning("[Rasterize] Previously selected entity (source cloud) has been hidden!");
		}

		MainWindow* mainWindow = MainWindow::TheInstance();
		if (mainWindow)
			MainWindow::TheInstance()->addToDB(rasterCloud);
		ccLog::Print(QString("[Rasterize] Cloud '%1' successfully exported").arg(rasterCloud->getName()));
	}
	
	generateCloudPushButton->blockSignals(false);
}

void ccRasterizeTool::generateImage() const
{
	if (!m_cloud || !m_grid.isValid())
		return;

	//default values
	double emptyCellsHeight = 0;
	double minHeight = m_grid.minHeight;
	double maxHeight = m_grid.maxHeight;
	//get real values
	EmptyCellFillOption fillEmptyCellsStrategy = getFillEmptyCellsStrategy(	emptyCellsHeight,
																			minHeight,
																			maxHeight);

	QImage bitmap8(m_grid.width,m_grid.height,QImage::Format_Indexed8);
	if (!bitmap8.isNull())
	{
		// Build a custom palette (gray scale)
		QVector<QRgb> palette(256);
		{
			for (unsigned i = 0; i < 256; i++)
				palette[i] = qRgba(i,i,i,255);
		}
		double maxColorComp = 255.99; //.99 --> to avoid round-off issues later!

		if (fillEmptyCellsStrategy == LEAVE_EMPTY)
		{
			palette[255] = qRgba(255,0,255,0); //magenta/transparent color for empty cells (in place of pure white)
			maxColorComp = 254.99;
		}

		bitmap8.setColorTable(palette);
		//bitmap8.fill(255);

		unsigned emptyCellColorIndex = 0;
		switch (fillEmptyCellsStrategy)
		{
		case LEAVE_EMPTY:
			emptyCellColorIndex = 255; //should be transparent!
			break;
		case FILL_MINIMUM_HEIGHT:
			emptyCellColorIndex = 0;
			break;
		case FILL_MAXIMUM_HEIGHT:
			emptyCellColorIndex = 255;
			break;
		case FILL_CUSTOM_HEIGHT:
			{
				double normalizedHeight = (emptyCellsHeight-minHeight)/(maxHeight-minHeight);
				//min and max should have already been updated with custom empty cell height!
				assert(normalizedHeight >= 0.0 && normalizedHeight <= 1.0);
				emptyCellColorIndex = static_cast<unsigned>(floor(normalizedHeight*maxColorComp));
			}
			break;
		case FILL_AVERAGE_HEIGHT:
		default:
			assert(false);
		}

		double range = maxHeight - minHeight;
		if (range < ZERO_TOLERANCE)
			range = 1.0;

		// Filling the image with grid values
		for (unsigned j=0; j<m_grid.height; ++j)
		{
			const RasterCell* aCell = m_grid.data[j];
			for (unsigned i=0; i<m_grid.width; ++i,++aCell)
			{
				if (aCell->nbPoints)
				{
					double normalizedHeight = (static_cast<double>(aCell->height) - minHeight)/range;
					assert(normalizedHeight >= 0.0 && normalizedHeight <= 1.0);
					unsigned char val = static_cast<unsigned char>(floor(normalizedHeight*maxColorComp));
					bitmap8.setPixel(i,m_grid.height-1-j,val);
				}
				else
				{
					bitmap8.setPixel(i,m_grid.height-1-j,emptyCellColorIndex);
				}
			}
		}

		//open file saving dialog
		{
			//add images output file filters
			QString filters;

			//we grab the list of supported image file formats (writing)
			QList<QByteArray> formats = QImageWriter::supportedImageFormats();
			if (formats.empty())
			{
				ccLog::Error("No image format supported by your system?!\n(check that the 'imageformats' directory is alongside CC executable)");
			}
			else
			{
				//we convert this list into a proper "filters" string
				for (int i=0; i<formats.size(); ++i)
					filters.append(QString("%1 image (*.%2)\n").arg(QString(formats[i].data()).toUpper()).arg(formats[i].data()));

				QSettings settings;
				settings.beginGroup(ccPS::HeightGridGeneration());
				QString imageSavePath = settings.value("savePathImage",QApplication::applicationDirPath()).toString();
				QString outputFilename = QFileDialog::getSaveFileName(0,"Save raster image",imageSavePath+QString("/raster_image.%1").arg(formats[0].data()),filters);

				if (!outputFilename.isNull())
				{
					if (bitmap8.save(outputFilename))
					{
						ccLog::Print(QString("[Rasterize] Image '%1' succesfully saved").arg(outputFilename));
						//save current export path to persistent settings
						settings.setValue("savePathImage",QFileInfo(outputFilename).absolutePath());
					}
					else
					{
						ccLog::Error("Failed to save image file!");
					}
				}
			}
		}
	}
	else
	{
		ccLog::Error("Failed to create output image! (not enough memory?)");
	}
}

#ifdef CC_GDAL_SUPPORT
//GDAL
#include <gdal.h>
#include <gdal_priv.h>
#include <cpl_string.h>

//local
#include "ui_rasterExportOptionsDlg.h"

//system
#include <assert.h>

class RasterExportOptionsDlg : public QDialog, public Ui::RasterExportOptionsDialog
{
public:

	RasterExportOptionsDlg(QWidget* parent = 0)
		: QDialog(parent)
		, Ui::RasterExportOptionsDialog()
	{
		setupUi(this);
		setWindowFlags(Qt::Tool);
	}
};

#endif

void ccRasterizeTool::generateRaster() const
{
#ifdef CC_GDAL_SUPPORT

	if (!m_cloud || !m_grid.isValid())
		return;

	GDALAllRegister();
	ccLog::PrintDebug("(GDAL drivers: %i)", GetGDALDriverManager()->GetDriverCount());

	const char *pszFormat = "GTiff";
	GDALDriver *poDriver = GetGDALDriverManager()->GetDriverByName(pszFormat);
	if (!poDriver)
	{
		ccLog::Error("[GDAL] Driver %s is not supported", pszFormat);
		return;
	}

	char** papszMetadata = poDriver->GetMetadata();
	if( !CSLFetchBoolean( papszMetadata, GDAL_DCAP_CREATE, FALSE ) )
	{
		ccLog::Error("[GDAL] Driver %s doesn't support Create() method", pszFormat);
		return;
	}

	QString outputFilename;
	{
		QSettings settings;
		settings.beginGroup(ccPS::HeightGridGeneration());
		QString imageSavePath = settings.value("savePathImage",QApplication::applicationDirPath()).toString();
		outputFilename = QFileDialog::getSaveFileName(0,"Save height grid raster",imageSavePath+QString("/raster.tif"),"geotiff (*.tif)");

		if (outputFilename.isNull())
			return;

		//save current export path to persistent settings
		settings.setValue("savePathImage",QFileInfo(outputFilename).absolutePath());
	}

	//which (and how many) bands shall we create?
	bool heightBand = true; //height by default
	bool densityBand = false;
	bool allSFBands = false;
	int sfBandIndex = -1; //scalar field index
	int totalBands = 0;

	bool interpolateSF = (getTypeOfSFInterpolation() != INVALID_PROJECTION_TYPE);
	ccPointCloud* pc = m_cloud->isA(CC_TYPES::POINT_CLOUD) ? static_cast<ccPointCloud*>(m_cloud) : 0;

	bool hasSF =  interpolateSF && pc && !m_grid.scalarFields.empty();
	
	RasterExportOptionsDlg reoDlg;
	reoDlg.dimensionsLabel->setText(QString("%1 x %2").arg(m_grid.width).arg(m_grid.height));
	reoDlg.exportHeightsCheckBox->setChecked(heightBand);
	reoDlg.exportDensityCheckBox->setChecked(densityBand);
	reoDlg.exportDisplayedSFCheckBox->setEnabled(hasSF);
	reoDlg.exportAllSFCheckBox->setEnabled(hasSF);
	reoDlg.exportAllSFCheckBox->setChecked(allSFBands);

	if (!reoDlg.exec())
		return;

	heightBand = reoDlg.exportHeightsCheckBox->isChecked();
	densityBand = reoDlg.exportDensityCheckBox->isChecked();
	if (hasSF)
	{
		assert(pc);
		allSFBands = reoDlg.exportAllSFCheckBox->isChecked() && hasSF;
		if (!allSFBands && reoDlg.exportDisplayedSFCheckBox->isChecked())
		{
			sfBandIndex = pc->getCurrentDisplayedScalarFieldIndex();
			if (sfBandIndex < 0)
				ccLog::Warning("[Rasterize] Cloud has no active (displayed) SF!");
		}
	}

	totalBands = heightBand ? 1 : 0;
	if (densityBand)
		++totalBands;
	if (allSFBands)
	{
		assert(hasSF);
		for (size_t i=0; i<m_grid.scalarFields.size(); ++i)
			if (m_grid.scalarFields[i])
				totalBands++;
	}
	
	if (totalBands == 0)
	{
		ccLog::Warning("[Rasterize] Warning, can't output a raster with no band! (check export parameters)");
		return;
	}

	//data type
	GDALDataType dataType = (std::max(sizeof(PointCoordinateType),sizeof(ScalarType)) > 4 ? GDT_Float64 : GDT_Float32);

	char **papszOptions = NULL;
	GDALDataset* poDstDS = poDriver->Create(qPrintable(outputFilename),
											static_cast<int>(m_grid.width),
											static_cast<int>(m_grid.height),
											totalBands,
											dataType, 
											papszOptions);

	if (!poDstDS)
	{
		ccLog::Error("[GDAL] Failed to create output raster (not enough memory?)");
		return;
	}

	ccBBox box = getCustomBBox();
	assert(box.isValid());

	//vertical dimension
	const unsigned char Z = getProjectionDimension();
	assert(Z >= 0 && Z <= 2);
	const unsigned char X = Z == 2 ? 0 : Z +1;
	const unsigned char Y = X == 2 ? 0 : X +1;

	double shiftX = box.minCorner().u[X];
	double shiftY = box.minCorner().u[Y];

	double stepX = m_grid.gridStep;
	double stepY = m_grid.gridStep;
	if (pc)
	{
		const CCVector3d& shift = pc->getGlobalShift();
		shiftX -= shift.u[X];
		shiftY -= shift.u[Y];

		double scale = pc->getGlobalScale();
		assert(scale != 0);
		stepX /= scale;
		stepY /= scale;
	}

	double adfGeoTransform[6] = {	shiftX,		//top left x
									stepX,		//w-e pixel resolution (can be negative)
									0,			//0
									shiftY,		//top left y
									0,			//0
									stepY		//n-s pixel resolution (can be negative)
	};

	poDstDS->SetGeoTransform( adfGeoTransform );

	//OGRSpatialReference oSRS;
	//oSRS.SetUTM( 11, TRUE );
	//oSRS.SetWellKnownGeogCS( "NAD27" );
	//char *pszSRS_WKT = NULL;
	//oSRS.exportToWkt( &pszSRS_WKT );
	//poDstDS->SetProjection( pszSRS_WKT );
	//CPLFree( pszSRS_WKT );

	double* scanline = (double*) CPLMalloc(sizeof(double)*m_grid.width);
	int currentBand = 0;

	//exort height band?
	if (heightBand)
	{
		GDALRasterBand* poBand = poDstDS->GetRasterBand(++currentBand);
		assert(poBand);
		poBand->SetColorInterpretation(GCI_Undefined);

		EmptyCellFillOption fillEmptyCellsStrategy = getFillEmptyCellsStrategy();

		double emptyCellHeight = 0;
		switch (fillEmptyCellsStrategy)
		{
		case LEAVE_EMPTY:
			emptyCellHeight = m_grid.minHeight-1.0;
			poBand->SetNoDataValue(emptyCellHeight); //should be transparent!
			break;
		case FILL_MINIMUM_HEIGHT:
			emptyCellHeight = m_grid.minHeight;
			break;
		case FILL_MAXIMUM_HEIGHT:
			emptyCellHeight = m_grid.maxHeight;
			break;
		case FILL_CUSTOM_HEIGHT:
			emptyCellHeight = getCustomHeightForEmptyCells();
			break;
		case FILL_AVERAGE_HEIGHT:
			emptyCellHeight = m_grid.meanHeight;
			break;
		default:
			assert(false);
		}

		for (unsigned j=0; j<m_grid.height; ++j)
		{
			const RasterCell* aCell = m_grid.data[j];
			for (unsigned i=0; i<m_grid.width; ++i,++aCell)
			{
				scanline[i] = aCell->nbPoints ? static_cast<double>(aCell->height) : emptyCellHeight;
			}

			if (poBand->RasterIO( GF_Write, 0, static_cast<int>(j), static_cast<int>(m_grid.width), 1, scanline, static_cast<int>(m_grid.width), 1, GDT_Float64, 0, 0 ) != CE_None)
			{
				ccLog::Error("[GDAL] An error occurred while writing the height band!");
				if (scanline)
					CPLFree(scanline);
				GDALClose( (GDALDatasetH) poDstDS );
				return;
			}
		}
	}

	//export density band
	if (densityBand)
	{
		GDALRasterBand* poBand = poDstDS->GetRasterBand(++currentBand);
		assert(poBand);
		poBand->SetColorInterpretation(GCI_Undefined);
		for (unsigned j=0; j<m_grid.height; ++j)
		{
			const RasterCell* aCell = m_grid.data[j];
			for (unsigned i=0; i<m_grid.width; ++i,++aCell)
			{
				scanline[i] = static_cast<double>(aCell->nbPoints);
			}

			if (poBand->RasterIO( GF_Write, 0, static_cast<int>(j), static_cast<int>(m_grid.width), 1, scanline, static_cast<int>(m_grid.width), 1, GDT_Float64, 0, 0 ) != CE_None)
			{
				ccLog::Error("[GDAL] An error occurred while writing the height band!");
				if (scanline)
					CPLFree(scanline);
				GDALClose( (GDALDatasetH) poDstDS );
				return;
			}
		}
	}

	//export SF bands
	if (allSFBands || sfBandIndex >= 0)
	{
		for (size_t k=0; k<m_grid.scalarFields.size(); ++k)
		{
			double* _sfGrid = m_grid.scalarFields[k];
			if (_sfGrid && (allSFBands || sfBandIndex == static_cast<int>(k))) //valid SF grid
			{
				GDALRasterBand* poBand = poDstDS->GetRasterBand(++currentBand);

				double sfNanValue = static_cast<double>(CCLib::ScalarField::NaN());
				poBand->SetNoDataValue(sfNanValue); //should be transparent!
				assert(poBand);
				poBand->SetColorInterpretation(GCI_Undefined);

				for (unsigned j=0; j<m_grid.height; ++j)
				{
					const RasterCell* aCell = m_grid.data[j];
					for (unsigned i=0; i<m_grid.width; ++i,++_sfGrid,++aCell)
					{
						scanline[i] = aCell->nbPoints ? *_sfGrid : sfNanValue;
					}

					if (poBand->RasterIO( GF_Write, 0, static_cast<int>(j), static_cast<int>(m_grid.width), 1, scanline, static_cast<int>(m_grid.width), 1, GDT_Float64, 0, 0 ) != CE_None)
					{
						//the corresponding SF should exist on the input cloud
						CCLib::ScalarField* formerSf = pc->getScalarField(static_cast<int>(k));
						assert(formerSf);
						ccLog::Error(QString("[GDAL] An error occurred while writing the '%1' scalar field band!").arg(formerSf->getName()));
						k = m_grid.scalarFields.size(); //quick stop
						break;
					}
				}
			}
		}
	}

	if (scanline)
		CPLFree(scanline);
	scanline = 0;

	/* Once we're done, close properly the dataset */
	GDALClose( (GDALDatasetH) poDstDS );

	ccLog::Print(QString("[Rasterize] Raster '%1' succesfully saved").arg(outputFilename));

#else
	assert(false);
	ccLog::Error("[Rasterize] GDAL not supported by this version! Can't generate a raster...");
#endif
}

void ccRasterizeTool::generateASCIIMatrix() const
{
	if (!m_cloud || !m_grid.isValid())
		return;

	QSettings settings;
	settings.beginGroup(ccPS::HeightGridGeneration());
	QString asciiGridSavePath = settings.value("savePathASCIIGrid",QApplication::applicationDirPath()).toString();

	//open file saving dialog
	QString filter("ASCII file (*.txt)");
	QString outputFilename = QFileDialog::getSaveFileName(0,"Save grid as ASCII file",asciiGridSavePath+QString("/raster_matrix.txt"),filter);
	if (outputFilename.isNull())
		return;

	FILE* pFile = fopen(qPrintable(outputFilename),"wt");
	if (!pFile)
	{
		ccLog::Warning(QString("[ccHeightGridGeneration] Failed to write '%1' file!").arg(outputFilename));
	}

	//default values
	double emptyCellsHeight = 0;
	double minHeight = m_grid.minHeight;
	double maxHeight = m_grid.maxHeight;
	//get real values
	EmptyCellFillOption fillEmptyCellsStrategy = getFillEmptyCellsStrategy(	emptyCellsHeight,
		minHeight,
		maxHeight);
	for (unsigned j=0; j<m_grid.height; ++j)
	{
		const RasterCell* aCell = m_grid.data[j];
		for (unsigned i=0; i<m_grid.width; ++i,++aCell)
			fprintf(pFile,"%.8f ", aCell->nbPoints ? aCell->height : emptyCellsHeight);

		fprintf(pFile,"\n");
	}

	fclose(pFile);
	pFile = 0;

	//save current export path to persistent settings
	settings.setValue("savePathASCIIGrid",QFileInfo(outputFilename).absolutePath());

	ccLog::Print(QString("[Rasterize] Raster matrix '%1' succesfully saved").arg(outputFilename));
}

void ccRasterizeTool::generateContours()
{
	if (!m_grid.isValid())
		return;

	double z = contourStartDoubleSpinBox->value();
	if (z > m_grid.maxHeight)
	{
		ccLog::Error("Start height is above maximum height!");
		return;
	}
	double zStep = contourStepDoubleSpinBox->value();
	assert(zStep > 0);
	unsigned levelCount = 1+static_cast<unsigned>(floor((m_grid.maxHeight-z)/zStep));

	removeContourLines();

	unsigned xDim = m_grid.width+2;
	unsigned yDim = m_grid.height+2;
	double* grid = new double[xDim * yDim];
	if (!grid)
	{
		ccLog::Error("Not enough memory!");
		if (m_window)
			m_window->redraw();
		return;
	}
	memset(grid,0,sizeof(double)*(xDim*yDim));

	//default values
	double emptyCellsHeight = 0;
	double minHeight = m_grid.minHeight;
	double maxHeight = m_grid.maxHeight;
	//get real values
	EmptyCellFillOption fillEmptyCellsStrategy = getFillEmptyCellsStrategy(	emptyCellsHeight,
																			minHeight,
																			maxHeight);

	//fill grid
	{
		if (fillEmptyCellsStrategy == LEAVE_EMPTY)
			emptyCellsHeight = minHeight-1.0;

		for (unsigned j=0; j<m_grid.height; ++j)
		{
			RasterCell* cell = m_grid.data[j];
			double* row = grid + (j+1)*xDim + 1;
			for (unsigned i=0; i<m_grid.width; ++i)
				row[i] = cell[i].nbPoints ? cell[i].height : emptyCellsHeight;
		}
	}

	bool memoryError = false;
	bool ignoreBorders = ignoreContourBordersCheckBox->isChecked();

	try
	{
		Isolines<double> iso(static_cast<int>(xDim),static_cast<int>(yDim));
		iso.createOnePixelBorder(grid,minHeight-1.0);
		//bounding box
		ccBBox box = getCustomBBox();
		assert(box.isValid());

		//vertical dimension
		const unsigned char Z = getProjectionDimension();
		assert(Z >= 0 && Z <= 2);
		const unsigned char X = Z == 2 ? 0 : Z +1;
		const unsigned char Y = X == 2 ? 0 : X +1;

		int minVertexCount = minVertexCountSpinBox->value();
		assert(minVertexCount >= 3);

		ccProgressDialog pDlg(true,this);
		pDlg.setMethodTitle("Contour plot");
		pDlg.setInfo(qPrintable(QString("Levels: %1\nCells: %2 x %3").arg(levelCount).arg(m_grid.width).arg(m_grid.height)));
		pDlg.start();
		pDlg.show();
		QApplication::processEvents();
		CCLib::NormalizedProgress nProgress(&pDlg,levelCount);

		while (z <= m_grid.maxHeight && !memoryError)
		{
			//extract contour lines for the current level
			iso.setThreshold(z);
			int lineCount = iso.find(grid);

			ccLog::PrintDebug(QString("[Rasterize][Isolines] z=%1 : %2 lines").arg(z).arg(lineCount));

			//convert them to poylines
			int realCount = 0;
			for (int i=0; i<lineCount; ++i)
			{
				int vertCount = iso.getContourLength(i);
				if (vertCount >= minVertexCount)
				{
					ccPointCloud* vertices = new ccPointCloud("vertices");
					ccPolyline* poly = new ccPolyline(vertices);
					poly->addChild(vertices);
					if (poly->reserve(vertCount) && vertices->reserve(vertCount))
					{
						unsigned localIndex = 0;
						for (int v=0; v<vertCount; ++v)
						{
							double x = iso.getContourX(i,v) - 1.0;
							double y = iso.getContourY(i,v) - 1.0;
							bool outside = ignoreBorders && (x < 1.0 || y < 1.0 || x+1.0 >= m_grid.width || y+1.0 >= m_grid.height);
							if (outside)
							{
								//split the polyline if necessary
								if (poly->size() != 0)
								{
									//reset local index!
									localIndex = 0;

									unsigned polySize = poly->size();
									if (polySize == 1)
									{
										//simply reset the polyline, we'll use it for the next part
										poly->clear();
										vertices->clear();
										if (!poly->reserve(vertCount - v) || !vertices->reserve(vertCount - v))
										{
											ccLog::Error("Not enough memory!");
											break;
										}
									}
									else if (v+2 >= vertCount)
									{
										//not enough vertex after this one to create a new polyline!
										//--> we can stop right away (the polyline will be taken care of later)
										break;
									}
									else
									{
										//close current polyline
										poly->resize(polySize);
										vertices->resize(vertices->size());

										//add the current poly chunk to DB
										poly->setName(QString("Contour line z=%1 (#%2)").arg(z).arg(++realCount));
										poly->setWidth(1);
										poly->setClosed(false);
										poly->setColor(ccColor::darkGrey);
										poly->showColors(true);
										vertices->setEnabled(false);

										if (m_window)
											m_window->addToOwnDB(poly);

										m_contourLines.push_back(poly);

										//create new poly for the rest
										vertices = new ccPointCloud("vertices");
										poly = new ccPolyline(vertices);
										poly->addChild(vertices);
										if (!poly->reserve(vertCount - v) || !vertices->reserve(vertCount - v))
										{
											ccLog::Error("Not enough memory!");
											break;
											//poly will be destroyed later as its size is 0!
											//delete poly;
											//poly = 0;
										}
									}
								}
								continue;
							}
							else
							{
								CCVector3 P;
								P.u[X] = static_cast<PointCoordinateType>(x * m_grid.gridStep + box.minCorner().u[X]);
								P.u[Y] = static_cast<PointCoordinateType>(y * m_grid.gridStep + box.minCorner().u[Y]);
								P.u[Z] = static_cast<PointCoordinateType>(z);

								vertices->addPoint(P);
								assert(localIndex < vertices->size());
								poly->addPointIndex(localIndex++);
							}
						}

						assert(poly);
						if (poly->size() > 1)
						{
							poly->setName(QString("Contour line z=%1 (#%2)").arg(z).arg(++realCount));
							poly->setWidth(1);
							poly->setClosed(poly->size() == vertCount); //if we have less vertices, it means we have 'chopped' the original contour
							poly->setColor(ccColor::darkGrey);
							poly->showColors(true);
							vertices->setEnabled(false);
						
							if (m_window)
								m_window->addToOwnDB(poly);

							m_contourLines.push_back(poly);
						}
						else
						{
							delete poly;
							poly = 0;
						}
					}
					else
					{
						delete poly;
						poly = 0;
						ccLog::Error("Not enough memory!");
						memoryError = true; //early stop
						break;
					}
				}
			}
			z += zStep;

			if (!nProgress.oneStep())
			{
				//process cancelled by user
				break;
			}
		}
	}
	catch(std::bad_alloc)
	{
		ccLog::Error("Not enough memory!");
	}

	ccLog::Print(QString("[Rasterize] %1 iso-lines generated (%2 levels)").arg(m_contourLines.size()).arg(levelCount));

	if (!m_contourLines.empty())
	{
		if (memoryError)
		{
			removeContourLines();
		}
		else
		{
			exportContoursPushButton->setEnabled(true);
			clearContoursPushButton->setEnabled(true);
		}
	}

	if (grid)
	{
		delete[] grid;
		grid = 0;
	}

	if (m_window)
		m_window->redraw();
}

void ccRasterizeTool::exportContourLines()
{
	MainWindow* mainWindow = MainWindow::TheInstance();
	if (!mainWindow || !m_cloud)
	{
		assert(false);
		return;
	}

	ccHObject* group = new ccHObject(QString("Contour plot(%1) [step=%2]").arg(m_cloud->getName()).arg(contourStepDoubleSpinBox->value()));
	for (size_t i=0; i<m_contourLines.size(); ++i)
	{
		ccPolyline* poly = m_contourLines[i];
		poly->setColor(ccColor::green);
		group->addChild(poly);
		if (m_window)
			m_window->removeFromOwnDB(poly);
	}
	m_contourLines.clear();

	group->setDisplay_recursive(m_cloud->getDisplay());
	mainWindow->addToDB(group);

	ccLog::Print(QString("Contour lines have been succesfully exported to DB (group name: %1)").arg(group->getName()));
}
