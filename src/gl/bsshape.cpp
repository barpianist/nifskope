#include "bsshape.h"
#include "settings.h"

#include "glscene.h"
#include "material.h"


BSShape::BSShape( Scene * s, const QModelIndex & b ) : Shape( s, b )
{
	
}

void BSShape::clear()
{
	Node::clear();

	verts.clear();
	norms.clear();
	tangents.clear();
	bitangents.clear();
	triangles.clear();
	coords.clear();
	colors.clear();
	test1.clear();
	test2.clear();
	test3.clear();
}

void BSShape::update( const NifModel * nif, const QModelIndex & index )
{
	Node::update( nif, index );

	if ( !iBlock.isValid() || !index.isValid() )
		return;

	if ( iBlock != index && !nif->inherits( index, "NiProperty" ) )
		return;

	//updateData |= index == iBlock;
	updateData = true;
	updateBounds |= updateData;

	bool isDynamic = nif->inherits( iBlock, "BSDynamicTriShape" );

	bool isDataOnSkin = false;
	auto skinID = nif->getLink( nif->getIndex( iBlock, "Skin" ) );
	auto vertexFlags = nif->get<quint16>( iBlock, "VF" );
	if ( (vertexFlags & 0x400) && nif->getUserVersion2() == 100 && skinID != -1 )
		isDataOnSkin = true;

	int vertCount = 0, triCount = 0, dataSize = 0;
	QModelIndex partBlock;

	if ( !isDataOnSkin ) {
		iVertData = nif->getIndex( iBlock, "Vertex Data" );
		iTriData = nif->getIndex( iBlock, "Triangles" );
		iData = iVertData;
		if ( !iVertData.isValid() || !iTriData.isValid() )
			return;

		numVerts = nif->get<int>( iBlock, "Num Vertices" );
		numTris = nif->get<int>( iBlock, "Num Triangles" );

		vertCount = nif->rowCount( iVertData );
		triCount = nif->rowCount( iTriData );

		numVerts = std::min( numVerts, vertCount );
		numTris = std::min( numTris, triCount );

		dataSize = nif->get<int>( iBlock, "Data Size" );
	} else {
		// For skinned geometry, the vertex data is stored on the NiSkinPartition
		// The triangles are split up among the partitions
		auto skinBlock = nif->getBlock( skinID, "NiSkinInstance" );
		partBlock = nif->getBlock( nif->getLink( skinBlock, "Skin Partition" ), "NiSkinPartition" );
		if ( !partBlock.isValid() )
			return;

		iVertData = nif->getIndex( partBlock, "Vertex Data" );
		iTriData = QModelIndex();
		iData = iVertData;

		dataSize = nif->get<int>( partBlock, "Data Size" );
		auto vertexSize = nif->get<int>( partBlock, "Vertex Size" );
		if ( !iVertData.isValid() || dataSize == 0 || vertexSize == 0 )
			return;

		numVerts = dataSize / vertexSize;
		vertCount = nif->rowCount( iVertData );
	}


	auto bsphere = nif->getIndex( iBlock, "Bounding Sphere" );
	if ( bsphere.isValid() ) {
		bsphereCenter = nif->get<Vector3>( bsphere, "Center" );
		bsphereRadius = nif->get<float>( bsphere, "Radius" );
	}

	if ( iBlock == index && dataSize > 0 ) {
		verts.clear();
		norms.clear();
		tangents.clear();
		bitangents.clear();
		triangles.clear();
		coords.clear();
		colors.clear();
		test1.clear();
		test2.clear();
		test3.clear();

		// For compatibility with coords QList
		QVector<Vector2> coordset;

		for ( int i = 0; i < numVerts; i++ ) {
			auto idx = nif->index( i, 0, iVertData );

			if ( !isDynamic )
				verts << nif->get<Vector3>( idx, "Vertex" );

			coordset << nif->get<HalfVector2>( idx, "UV" );

			// Bitangent X
			auto bitX = nif->getValue( nif->getIndex( idx, "Bitangent X" ) ).toFloat();
			// Bitangent Y/Z
			auto bitYi = nif->getValue( nif->getIndex( idx, "Bitangent Y" ) ).toCount();
			auto bitZi = nif->getValue( nif->getIndex( idx, "Bitangent Z" ) ).toCount();
			auto bitY = (double( bitYi ) / 255.0) * 2.0 - 1.0;
			auto bitZ = (double( bitZi ) / 255.0) * 2.0 - 1.0;

			auto n = nif->get<ByteVector3>( idx, "Normal" );
			norms += n;

			auto t = nif->get<ByteVector3>( idx, "Tangent" );
			tangents += t;
			bitangents += Vector3( bitX, bitY, bitZ );

			auto vcIdx = nif->getIndex( idx, "Vertex Colors" );
			if ( vcIdx.isValid() ) {
				colors += nif->get<ByteColor4>( vcIdx );
			}
		}

		if ( isDynamic ) {
			auto dynVerts = nif->getArray<Vector4>( iBlock, "Vertices" );
			for ( const auto & v : dynVerts )
				verts << Vector3( v );
		}

		// Add coords as first set of QList
		coords.append( coordset );

		if ( !isDataOnSkin ) {
			triangles = nif->getArray<Triangle>( iTriData );
			triangles = triangles.mid( 0, numTris );
		} else {
			auto partIdx = nif->getIndex( partBlock, "Partition" );
			for ( int i = 0; i < nif->rowCount( partIdx ); i++ )
				triangles << nif->getArray<Triangle>( nif->index( i, 0, partIdx ), "Triangles" );
		}
	}

	// Update shaders from this mesh's shader property
	updateShaderProperties( nif );

	if ( bssp )
		isVertexAlphaAnimation = bssp->hasSF2( ShaderFlags::SLSF2_Tree_Anim );

	if ( isVertexAlphaAnimation ) {
		for ( int i = 0; i < colors.count(); i++ )
			colors[i].setRGBA( colors[i].red(), colors[i].green(), colors[i].blue(), 1.0 );
	}
}

QModelIndex BSShape::vertexAt( int idx ) const
{
	auto nif = static_cast<const NifModel *>(iBlock.model());
	if ( !nif )
		return QModelIndex();

	auto iVertexData = nif->getIndex( iBlock, "Vertex Data" );
	auto iVertex = iVertexData.child( idx, 0 );

	return nif->getIndex( iVertex, "Vertex" );
}


void BSShape::transform()
{
	if ( isHidden() )
		return;

	const NifModel * nif = static_cast<const NifModel *>(iBlock.model());
	if ( !nif || !iBlock.isValid() ) {
		clear();
		return;
	}

	if ( updateData ) {
		
	}

	Node::transform();
}

void BSShape::transformShapes()
{
	if ( isHidden() )
		return;

	Node::transformShapes();
}

void BSShape::drawShapes( NodeList * secondPass, bool presort )
{
	if ( isHidden() )
		return;

	glPointSize( 8.5 );

	// TODO: Only run this if BSXFlags has "EditorMarkers present" flag
	if ( !(scene->options & Scene::ShowMarkers) && name.startsWith( "EditorMarker" ) )
		return;

	if ( Node::SELECTING ) {
		if ( scene->selMode & Scene::SelObject ) {
			int s_nodeId = ID2COLORKEY( nodeId );
			glColor4ubv( (GLubyte *)&s_nodeId );
		} else {
			glColor4f( 0, 0, 0, 1 );
		}
	}

	// Draw translucent meshes in second pass
	AlphaProperty * aprop = findProperty<AlphaProperty>();
	Material * mat = (bssp) ? bssp->mat() : nullptr;

	drawSecond |= aprop && aprop->blend();
	drawSecond |= mat && mat->bDecal;

	if ( secondPass && drawSecond ) {
		secondPass->add( this );
		return;
	}

	glPushMatrix();
	glMultMatrix( viewTrans() );

	// Render polygon fill slightly behind alpha transparency and wireframe
	if ( !drawSecond ) {
		glEnable( GL_POLYGON_OFFSET_FILL );
		glPolygonOffset( 1.0f, 2.0f );
	}

	glEnableClientState( GL_VERTEX_ARRAY );
	glVertexPointer( 3, GL_FLOAT, 0, verts.data() );

	if ( !Node::SELECTING ) {
		glEnableClientState( GL_NORMAL_ARRAY );
		glNormalPointer( GL_FLOAT, 0, norms.data() );

		bool doVCs = (bssp && (bssp->getFlags2() & ShaderFlags::SLSF2_Vertex_Colors));


		Color4 * c = nullptr;
		if ( colors.count() && (scene->options & Scene::DoVertexColors) && doVCs ) {
			c = colors.data();
		}

		if ( c ) {
			glEnableClientState( GL_COLOR_ARRAY );
			glColorPointer( 4, GL_FLOAT, 0, c );
		} else {
			glColor( Color3( 1.0f, 1.0f, 1.0f ) );
		}
	}


	if ( !Node::SELECTING )
		shader = scene->renderer->setupProgram( this, shader );
	
	if ( isDoubleSided ) {
		glCullFace( GL_FRONT );
		glDrawElements( GL_TRIANGLES, triangles.count() * 3, GL_UNSIGNED_SHORT, triangles.data() );
		glCullFace( GL_BACK );
	}

	glDrawElements( GL_TRIANGLES, triangles.count() * 3, GL_UNSIGNED_SHORT, triangles.data() );

	if ( !Node::SELECTING )
		scene->renderer->stopProgram();

	glDisableClientState( GL_VERTEX_ARRAY );
	glDisableClientState( GL_NORMAL_ARRAY );
	glDisableClientState( GL_COLOR_ARRAY );

	glDisable( GL_POLYGON_OFFSET_FILL );


	if ( scene->selMode & Scene::SelVertex ) {
		drawVerts();
	}

	glPopMatrix();
}

void BSShape::drawVerts() const
{
	glDisable( GL_LIGHTING );
	glNormalColor();

	glBegin( GL_POINTS );

	for ( int i = 0; i < numVerts; i++ ) {
		if ( Node::SELECTING ) {
			int id = ID2COLORKEY( ( shapeNumber << 16 ) + i );
			glColor4ubv( (GLubyte *)&id );
		}
		glVertex( verts.value( i ) );
	}

	// Highlight selected vertex
	if ( !Node::SELECTING && iBlock == scene->currentBlock ) {
		auto idx = scene->currentIndex;
		if ( idx.data( Qt::DisplayRole ).toString() == "Vertex" ) {
			glHighlightColor();
			glVertex( verts.value( idx.parent().row() ) );
		}
	}

	glEnd();
}

void BSShape::drawSelection() const
{
	if ( scene->options & Scene::ShowNodes )
		Node::drawSelection();

	if ( isHidden() || !(scene->selMode & Scene::SelObject) )
		return;

	// Is the current block extra data
	bool extraData = false;

	auto nif = static_cast<const NifModel *>(scene->currentIndex.model());
	if ( !nif )
		return;

	// Set current block and detect if extra data
	auto currentBlock = nif->getBlockName( scene->currentBlock );
	if ( currentBlock == "BSSkin::BoneData" || currentBlock == "BSPackedCombinedSharedGeomDataExtra" )
		extraData = true;

	// Don't do anything if this block is not the current block
	//	or if there is not extra data
	if ( scene->currentBlock != iBlock && !extraData )
		return;

	auto n = scene->currentIndex.data( NifSkopeDisplayRole ).toString();

	glPushMatrix();
	glMultMatrix( viewTrans() );

	glDepthFunc( GL_LEQUAL );

	glDisable( GL_LIGHTING );
	glDisable( GL_COLOR_MATERIAL );
	glDisable( GL_TEXTURE_2D );
	glDisable( GL_NORMALIZE );
	glEnable( GL_DEPTH_TEST );
	glDepthMask( GL_FALSE );
	glEnable( GL_BLEND );
	glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
	glDisable( GL_ALPHA_TEST );

	glDisable( GL_CULL_FACE );

	// TODO: User Settings
	int lineWidth = 1.5;
	int pointSize = 5.0;

	glLineWidth( lineWidth );
	glPointSize( pointSize );

	glNormalColor();

	glEnable( GL_POLYGON_OFFSET_FILL );
	glPolygonOffset( -1.0f, -2.0f );

	float normalScale = bounds().radius / 20;
	normalScale /= 2.0f;

	if ( normalScale < 0.1f )
		normalScale = 0.1f;

	auto pBlock = nif->getBlock( nif->getParent( scene->currentBlock ) );
	bool vp = scene->currentIndex.parent().data( NifSkopeDisplayRole ).toString() == "Vertex Data";

	// Draw All Verts lambda
	auto allv = [this]( float size ) {
		glPointSize( size );
		glBegin( GL_POINTS );

		for ( int j = 0; j < verts.count(); j++ )
			glVertex( verts.value( j ) );

		glEnd();
	};

	if ( n == "Bounding Sphere" && !extraData ) {
		Vector3 bvC = nif->get<Vector3>( scene->currentIndex.child( 0, 2 ) );
		float bvR = nif->get<float>( scene->currentIndex.child( 1, 2 ) );

		if ( bvR > 0.0 ) {
			glColor4f( 1, 1, 1, 0.33 );
			drawSphereSimple( bvC, bvR, 72 );
		}
	}
	
	if ( currentBlock == "BSPackedCombinedSharedGeomDataExtra" && pBlock == iBlock ) {
		QVector<QModelIndex> idxs;
		if ( n == "Bounding Sphere" ) {
			idxs += scene->currentIndex;
		} else if ( n == "BSPackedCombinedSharedGeomDataExtra" ) {
			auto data = nif->getIndex( scene->currentIndex, "Object Data" );
			int dataCt = nif->rowCount( data );

			for ( int i = 0; i < dataCt; i++ ) {
				auto d = data.child( i, 0 );

				int numC = nif->get<int>( d, "Num Combined" );
				auto c = nif->getIndex( d, "Combined" );
				int cCt = nif->rowCount( c );

				for ( int j = 0; j < cCt; j++ ) {
					idxs += nif->getIndex( c.child( j, 0 ), "Bounding Sphere" );
				}
			}
		}

		if ( !idxs.count() ) {
			glPopMatrix();
			return;
		}

		Vector3 pTrans = nif->get<Vector3>( pBlock, "Translation" );
		auto iBSphere = nif->getIndex( pBlock, "Bounding Sphere" );
		Vector3 pbvC = nif->get<Vector3>( iBSphere.child( 0, 2 ) );
		float pbvR = nif->get<float>( iBSphere.child( 1, 2 ) );

		if ( pbvR > 0.0 ) {
			glColor4f( 0, 1, 0, 0.33 );
			drawSphereSimple( pbvC, pbvR, 72 );
		}

		glPopMatrix();

		for ( auto idx : idxs ) {
			Matrix mat = nif->get<Matrix>( idx.parent(), "Rotation" );
			//auto trans = nif->get<Vector3>( idx.parent(), "Translation" );
			float scale = nif->get<float>( idx.parent(), "Scale" );

			Vector3 bvC = nif->get<Vector3>( idx, "Center" );
			float bvR = nif->get<float>( idx, "Radius" );

			Transform t;
			t.rotation = mat.inverted();
			t.translation = bvC;
			t.scale = scale;

			glPushMatrix();
			glMultMatrix( scene->view * t );

			if ( bvR > 0.0 ) {
				glColor4f( 1, 1, 1, 0.33 );
				drawSphereSimple( Vector3( 0, 0, 0 ), bvR, 72 );
			}

			glPopMatrix();
		}

		glPushMatrix();
		glMultMatrix( viewTrans() );
	}

	if ( n == "Vertex Data" || n == "Vertex" ) {
		allv( 5.0 );

		int s = -1;
		if ( n == "Vertex Data" && vp ) {
			s = scene->currentIndex.row();
		} else if ( n == "Vertex" ) {
			s = scene->currentIndex.parent().row();
		}

		if ( s >= 0 ) {
			glPointSize( 10 );
			glDepthFunc( GL_ALWAYS );
			glHighlightColor();
			glBegin( GL_POINTS );
			glVertex( verts.value( s ) );
			glEnd();
		}
	} 
	
	glPolygonMode( GL_FRONT_AND_BACK, GL_LINE );

	// Draw Lines lambda
	auto lines = [this, &normalScale, &allv, &lineWidth]( const QVector<Vector3> & v ) {
		allv( 7.0 );

		int s = scene->currentIndex.parent().row();
		glBegin( GL_LINES );

		for ( int j = 0; j < verts.count() && j < v.count(); j++ ) {
			glVertex( verts.value( j ) );
			glVertex( verts.value( j ) + v.value( j ) * normalScale * 2 );
			glVertex( verts.value( j ) );
			glVertex( verts.value( j ) - v.value( j ) * normalScale / 2 );
		}

		glEnd();

		if ( s >= 0 ) {
			glDepthFunc( GL_ALWAYS );
			glHighlightColor();
			glLineWidth( 3.0 );
			glBegin( GL_LINES );
			glVertex( verts.value( s ) );
			glVertex( verts.value( s ) + v.value( s ) * normalScale * 2 );
			glVertex( verts.value( s ) );
			glVertex( verts.value( s ) - v.value( s ) * normalScale / 2 );
			glEnd();
			glLineWidth( lineWidth );
		}
	};
	
	// Draw Normals
	if ( n.contains( "Normal" ) ) {
		lines( norms );
	}

	// Draw Tangents
	if ( n.contains( "Tangent" ) ) {
		lines( tangents );
	}

	// Draw Triangles
	if ( n == "Triangles" ) {
		int s = scene->currentIndex.row();
		if ( s >= 0 ) {
			glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );
			glHighlightColor();

			Triangle tri = triangles.value( s );
			glBegin( GL_TRIANGLES );
			glVertex( verts.value( tri.v1() ) );
			glVertex( verts.value( tri.v2() ) );
			glVertex( verts.value( tri.v3() ) );
			glEnd();
			glPolygonMode( GL_FRONT_AND_BACK, GL_LINE );
		}
	}

	// Draw Segments/Subsegments
	if ( n == "Segment" || n == "Sub Segment" || n == "Num Primitives" ) {
		auto idx = scene->currentIndex;
		int s;
		if ( n != "Num Primitives" ) {
			idx = scene->currentIndex.child( 1, 0 );
		}
		s = idx.row();

		auto nif = static_cast<const NifModel *>(idx.model());

		auto off = idx.sibling( s - 1, 2 ).data().toInt() / 3;
		auto cnt = idx.sibling( s, 2 ).data().toInt();

		auto numRec = idx.sibling( s + 2, 2 ).data().toInt();

		QVector<QColor> cols = { { 255, 0, 0, 128 }, { 0, 255, 0, 128 }, { 0, 0, 255, 128 }, { 255, 255, 0, 128 },
								{ 0, 255, 255, 128 }, { 255, 0, 255, 128 }, { 255, 255, 255, 128 } 
		};

		glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );

		int maxTris = triangles.count();

		if ( numRec > 0 ) {
			auto recs = idx.sibling( s + 3, 0 );
			for ( int i = 0; i < numRec; i++ ) {
				auto subrec = recs.child( i, 0 );
				auto off = subrec.child( 0, 2 ).data().toInt() / 3;
				auto cnt = subrec.child( 1, 2 ).data().toInt();

				int j = off;
				for ( j; j < cnt + off; j++ ) {
					if ( j >= maxTris )
						continue;

					glColor( Color4(cols.value( i % 7 )) );
					Triangle tri = triangles[j];
					glBegin( GL_TRIANGLES );
					glVertex( verts.value( tri.v1() ) );
					glVertex( verts.value( tri.v2() ) );
					glVertex( verts.value( tri.v3() ) );
					glEnd();
				}
			}
		} else {
			glColor( Color4(cols.value( scene->currentIndex.row() % 7 )) );

			int i = off;
			for ( i; i < cnt + off; i++ ) {
				if ( i >= maxTris )
					continue;

				Triangle tri = triangles[i];
				glBegin( GL_TRIANGLES );
				glVertex( verts.value( tri.v1() ) );
				glVertex( verts.value( tri.v2() ) );
				glVertex( verts.value( tri.v3() ) );
				glEnd();
			}
		}
		glPopMatrix();
		return;
	}

	// Bone bounds lambda
	auto boneSphere = [nif]( QModelIndex b ) {
		auto bSphere = nif->getIndex( b, "Bounding Sphere" );
		auto bTrans = nif->get<Vector3>( b, "Translation" );
		auto bRot = nif->get<Matrix>( b, "Rotation" );

		Vector3 bvC = nif->get<Vector3>( bSphere.child( 0, 2 ) );
		float bvR = nif->get<float>( bSphere.child( 1, 2 ) );

		if ( bvR > 0.0 ) {
			glColor4f( 1, 1, 1, 0.33 );
			auto pos = bRot.inverted() * (bvC - bTrans);
			drawSphereSimple( pos, bvR, 36 );
		}
	};

	// Draw all bones' bounding spheres
	if ( n == "BSSkin::BoneData" ) {
		auto bData = scene->currentBlock;
		auto skin = nif->getParent( bData );
		auto shape = nif->getParent( skin );

		if ( nif->getBlock( shape ) == iBlock ) {
			auto bones = nif->getIndex( bData, "Bones" );
			int ct = nif->rowCount( bones );

			for ( int i = 0; i < ct; i++ ) {
				auto b = bones.child( i, 0 );
				boneSphere( b );
			}
		}
		glPopMatrix();
		return;
	}

	// Draw bone bounding sphere
	if ( n == "Bones" && currentBlock == "BSSkin::BoneData" ) {
		boneSphere( scene->currentIndex );
		glPopMatrix();
		return;
	}

	// General wireframe
	if ( (scene->currentIndex != iVertData) && !vp ) {
		glLineWidth( 1.6f );
		glNormalColor();
		for ( const Triangle& tri : triangles ) {
			glBegin( GL_TRIANGLES );
			glVertex( verts.value( tri.v1() ) );
			glVertex( verts.value( tri.v2() ) );
			glVertex( verts.value( tri.v3() ) );
			glEnd();
		}
	}

	glDisable( GL_POLYGON_OFFSET_FILL );

	glPopMatrix();
}

BoundSphere BSShape::bounds() const
{
	if ( updateBounds ) {
		updateBounds = false;
		if ( verts.count() ) {
			boundSphere = BoundSphere( verts );
		} else {
			boundSphere = BoundSphere( bsphereCenter, bsphereRadius );
		}
	}

	return worldTrans() * boundSphere;
}

bool BSShape::isHidden() const
{
	return Node::isHidden();
}
