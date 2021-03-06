#include "bvhtest.h"

#include "auxiliary.h"

#include <deque>
#include <unordered_set>
#include "config.h"

#include "tiny_obj_loader.h"


#include "BVH/hlbvh.h"
#include "BVH/treeletBVH.h"

#include <iostream>

using namespace MCPT;
using namespace MCPT::Auxiliary;


namespace {
	bool intersectBox(const Ray& r, const BoundingBox& box, float tmin, float& tans) {
		cl_float4 off1 = (box.bbmin - r.origin) / r.direction;
		cl_float4 off2 = (box.bbmax - r.origin) / r.direction;

		cl_float4 tempMin = min(off1, off2);
		cl_float4 tempMax = max(off1, off2);
		cl_float tnear = std::max(std::max(tempMin.x, tempMin.y), tempMin.z);
		cl_float tfar = std::min(std::min(tempMax.x, tempMax.y), tempMax.z);
		if (tfar < tnear || tfar < tmin) return false;
		tans = tnear;
		return true;
	}

	bool intersectPlane(const cl_float4& normal, Ray* ray, float* tans, float tmin) {
		float dotans = dot(ray->direction, normal);

		if (dotans == 0) return false;
		float t = (normal.w - dot(normal, ray->origin)) / dotans;

		if (t <= tmin) return false;
		*tans = t;
		return true;
	}


	std::vector<Triangle> loadObj(const std::string& directory, const std::string& objname) {
		std::vector<Triangle> ansTrg;

		tinyobj::attrib_t attrib;
		std::vector<tinyobj::shape_t> shapes;
		std::vector<tinyobj::material_t> materials;

		tinyobj::LoadObj(&attrib, &shapes, &materials, nullptr, nullptr, (directory + objname).c_str(), directory.c_str());

		auto packFloat = [&](size_t* tid) -> Triangle {
			Triangle ans = { 0 };
			for (size_t i = 0; i < 3; ++i) {
				for (size_t j = 0; j < 3; ++j) {
					ans.v[i].s[j] = attrib.vertices[tid[i] * 3 + j];
				}
			}
			return ans;
		};
		auto packTriangle = [&](const tinyobj::shape_t& x) -> void {
			for (size_t i = 0; i < x.mesh.num_face_vertices.size(); ++i) {
				size_t offset = i * 3;

				size_t tid[3];
				tid[0] = x.mesh.indices[offset].vertex_index;
				tid[1] = x.mesh.indices[offset + 1].vertex_index;
				tid[2] = x.mesh.indices[offset + 2].vertex_index;

				ansTrg.push_back(packFloat(tid));
			}
		};

		for (auto& shape : shapes) {
			packTriangle(shape);
		}

		return ansTrg;
	}

}



namespace {

	cl::Program prog;
	cl::Kernel kernel;

}







namespace MCPT::BVH::TEST {

	float SAH(const std::vector<BVHNode>& node) {
		double sahAns = 0.0f;
		for (size_t i = 0; i < (node.size() >> 1); ++i) {
			sahAns += Cinn * AREA(node[i].bbmin,node[i].bbmax);
		}
		for (size_t i = (node.size() >> 1); i < node.size(); ++i) {
			sahAns += Ctri * AREA(node[i].bbmin,node[i].bbmax);
		}
		sahAns /= AREA(node[0].bbmin,node[0].bbmax);

		return sahAns;
	}
	
	std::vector<float> epoTValue;
	std::vector<float> bgData;

	float EPO(const std::vector<BVHNode>& node, const std::vector<Triangle>& triangles) {
		float EPO = 0.0f;
		std::unordered_set<size_t> ancestor;

		auto GETGEO = [&](size_t leafID) -> const Triangle& {
			return triangles[node[leafID].left];
		};
		
		auto POINTINBOX = [](const cl_float4& p, const BoundingBox& box) -> bool {
			if ((p.x >= box.bbmin.x && p.x <= box.bbmax.x)
				&& (p.y >= box.bbmin.y && p.y <= box.bbmax.y)
				&& (p.z >= box.bbmin.z && p.z <= box.bbmax.z))
			{
				return true;
			}
			return false;
		};
		auto TRIANGLEAREA = [](const Triangle& geo) -> float {
			return len(cross(geo.v[1] - geo.v[0], geo.v[2] - geo.v[0])) / 2;
		};

		auto ROUNDTR = [](std::vector<cl_float4>& points, int axis, float pos, int sign) -> void {
			std::vector<cl_float4> ans;
			std::vector<int> inside;

			float tans;

			if (sign > 0) {
				for (auto& p : points) {
					inside.push_back(p.s[axis] >= pos);
				}
			}
			else {
				for (auto& p : points) {
					inside.push_back(p.s[axis] <= pos);
				}
			}
			if (points.empty()) return;
			for (int i = 0; i < points.size(); ++i) {
				auto i_1 = (i + 1 == points.size()) ? (0) : (i + 1);
				if (!inside[i] && !inside[i_1]) {
					continue;
				}
				else if (inside[i] && inside[i_1]) {
					ans.push_back(points[i]);
					continue;
				}
				else {
					if (inside[i]) {
						ans.push_back(points[i]);
					}
					cl_float4 direction = points[i_1] - points[i];
					tans = (pos - points[i].s[axis]) / direction.s[axis];
					ans.push_back(points[i] + tans * direction);
				}
			}

			points.swap(ans);
		};
		auto pArea = [](const std::vector<cl_float4>& points) -> float {
			float ans = 0.0f;
			int sz = points.size();
			if (sz < 2) {
				return ans;
			}
			for (int i = 1; i < sz - 1; ++i) {
				auto x1 = points[i] - points[0];
				auto x2 = points[i + 1] - points[0];
				ans += len(cross(x1, x2)) / 2;
			}
			return ans;
		};
		auto INTERSECTAREA = [&](const Triangle& geo, BoundingBox& box) -> float {
			Triangle tr = geo;
			tr.v[0].w = 1;
			tr.v[1].w = 1;
			tr.v[2].w = 1;

			bool inside[3];
			inside[0] = POINTINBOX(tr.v[0], box);
			inside[1] = POINTINBOX(tr.v[1], box);
			inside[2] = POINTINBOX(tr.v[2], box);

			if (inside[0] && inside[1] && inside[2]) {
				return TRIANGLEAREA(tr);
			}

			std::vector< cl_float4 > points = { tr.v[0],tr.v[1],tr.v[2] };
			

			ROUNDTR(points, 0, box.bbmin.x, 1);
			ROUNDTR(points, 1, box.bbmin.y, 1);
			ROUNDTR(points, 2, box.bbmin.z, 1);

			ROUNDTR(points, 0, box.bbmax.x, -1);
			ROUNDTR(points, 1, box.bbmax.y, -1);
			ROUNDTR(points, 2, box.bbmax.z, -1);

			return pArea(points);
		};

		std::deque<size_t> toBeDone;
		auto TopDown = [&](size_t leafID, size_t rootID) -> void {
			auto& geo = GETGEO(leafID);
			auto box = BoundingBox({ node[rootID].bbmin,node[rootID].bbmax });

			if (ancestor.find(rootID) == ancestor.end()) {
				float epovalue = INTERSECTAREA(geo, box);
				if (epovalue > 0) {

					EPO += epovalue * ((rootID >= (node.size() >> 1)) ? Ctri : Cinn); // / AREA2(box);   // / areas[rootID];

					if (node[rootID].left != node[rootID].right) {
						toBeDone.push_back(node[rootID].left);
						toBeDone.push_back(node[rootID].right);
					}
				}
			}
			else {
				if (node[rootID].left != node[rootID].right) {
					toBeDone.push_back(node[rootID].left);
					toBeDone.push_back(node[rootID].right);
				}
			}
		};

		float percent = 0.05f;
		float step = 0.05f;
		int sz = (node.size() >> 1);

		float prevEPO = 0.0f;
		for (size_t i = sz; i < node.size(); ++i) {
			ancestor.insert(i);

			size_t j = i;
			while (node[j].parent != -1) {
				ancestor.insert(node[j].parent);
				j = node[j].parent;
			}

			toBeDone.push_back(0);
			while (!toBeDone.empty()) {
				auto front = toBeDone.front();
				toBeDone.pop_front();
				TopDown(i, front);
			}

			epoTValue.push_back(EPO - prevEPO);
			prevEPO = EPO;

			ancestor.clear();
			
			if ((i - sz) > (sz + 1) * percent) {
				std::cout << "Now EPO Test: " << (i - sz) << std::endl;
				percent += step;
			}
		}
		std::cout << "epo total " << EPO << std::endl;

		float totalAREA = 0.0;
		for (size_t i = (node.size() >> 1); i < node.size(); ++i) {
			totalAREA += TRIANGLEAREA(GETGEO(i));
		}
		EPO /= totalAREA;
		std::cout << "Total Area: " << totalAREA << std::endl;
		return EPO;
	}

	float EPO_GPU(cl::Buffer bvh, cl::Buffer triangles) {
		static auto initer = []() {
			prog = OpenCLBasic::createProgramFromFileWithHeader("./kernels/EPO.cl", "objdef.h");
			kernel = OpenCLBasic::createKernel(prog, "calculateEPO");
			return 0;
		}();

		int triCount = triangles.getInfo<CL_MEM_SIZE>() / sizeof(Triangle);
		cl::Buffer epoBuffer = OpenCLBasic::newBuffer<float>(triCount);
		cl::Buffer triAreaBuffer = OpenCLBasic::newBuffer<float>(triCount);

		OpenCLBasic::setKernelArg(kernel, bvh, triangles, epoBuffer, triAreaBuffer, triCount);
		OpenCLBasic::enqueueNDRange(kernel, triCount, cl::NullRange);

		std::vector<float> epoAns;
		std::vector<float> areaAns;
		epoAns.resize(triCount);
		areaAns.resize(triCount);
		OpenCLBasic::readBuffer(epoBuffer, epoAns.data());
		OpenCLBasic::readBuffer(triAreaBuffer, areaAns.data());


		double count = 0.0;
		for (int i = 0; i < epoAns.size(); ++i) {
			count += epoAns[i];
		}
		double area = 0.0;
		for (int i = 0; i < areaAns.size(); ++i) {
			area += areaAns[i];
		}
		//std::cout << "GPU: " << count << " " << area << std::endl;
		count /= area;
		return count;
	}


	float LCV(const std::vector<BVHNode>& node, const Camera& camera) {
		std::vector<int> stack;
		stack.reserve(256);

		auto countLeafIntersect = [&](const Ray& r) -> size_t {
			size_t ans = 0;
			auto* pt = node.data();
			stack.clear();
			cl_float temp;
		AGAIN:
			auto box = BoundingBox({ pt->bbmin, pt->bbmax });
			if (intersectBox(r, box, 0.001f, temp)) {
				if (pt->left == pt->right) {
					++ans;
					if (stack.empty()) goto END;
					pt = node.data() + stack.back();
					stack.pop_back();
					goto AGAIN;
				}
				else {
					stack.push_back(pt->right);
					pt = node.data() + pt->left;
					goto AGAIN;
				}
			}
			if (!stack.empty()) {
				pt = node.data() + stack.back();
				stack.pop_back();
				goto AGAIN;
			}
		END:
			return ans;
		};

		auto GENERATERAY = [&](std::vector<Ray>& rays) {
			int WIDTH = Config::WIDTH();
			int HEIGHT = Config::HEIGHT();
			//Camera camera;
			//switch (modelID) {
			//case 0:
			//	camera = Camera(glm::vec3(-470.0f, 359.0f, 316.0f), glm::vec3(0.0f, 1.0f, 0.0f), -40.0f, -26.2f);
			//	break;
			//case 1:
			//	camera = Camera(glm::vec3(0.0f, 0.1f, 2.7f));
			//	break;
			//case 2:
			//	camera = Camera(glm::vec3(0.0f, 0.2f, 0.7f));
			//	break;
			//case 3:
			//	camera = Camera(glm::vec3(-0.6f, -1.0f, 10.0f));
			//	break;
			//case 4:
			//	//22.857025 16.192675 12.377654   -148.400024 -22.00001122.857025 16.192675 12.377654   -148.400024 -22.000011
			//	camera = Camera(glm::vec3(22.857025f, 16.192675f, 12.377654f), glm::vec3(0.0f, 1.0f, 0.0f), -148.0f, -22.0f);
			//	break;
			//case 5://Sponza
			//	camera = Camera(glm::vec3(302.353668f, 410.221863f, -135.232559f), glm::vec3(0.0f, 1.0f, 0.0f), -209.0f, -20.0f);
			//	break;
			//case 6://San_Miguel
			//	camera = Camera(glm::vec3(25.0f, 2.2f, 1.9f), glm::vec3(0.0f, 1.0f, 0.0f), -209.0f, -20.0f);
			//	break;
			//}

			/*cl_float4 pos = { camera.Position.x,camera.Position.y,camera.Position.z,1.0f };
			cl_float4 up = { camera.Up.x,camera.Up.y,camera.Up.z,0 };
			cl_float4 right = { camera.Right.x,camera.Right.y,camera.Right.z,0 };

			cl_float4 front;
			auto Yaw = camera.Yaw;
			auto Pitch = camera.Pitch;

			front.x = cos(glm::radians(Yaw)) * cos(glm::radians(Pitch));
			front.y = sin(glm::radians(Pitch));
			front.z = sin(glm::radians(Yaw)) * cos(glm::radians(Pitch));
			front.w = 0;*/

			/*for (int i = 0; i < WIDTH; ++i) {
				for (int j = 0; j < HEIGHT; ++j) {
					float temp1 = (i + 0.5f) / WIDTH - 0.5f;
					float temp2 = (j + 0.5f) / HEIGHT - 0.5f;
					Ray r;
					r.origin = pos;
					r.direction = front + temp1 * right + temp2 * up;
					rays.push_back(std::move(r));
				}
			}*/

			for (int i = 0; i < WIDTH; ++i) {
				for (int j = 0; j < HEIGHT; ++j) {
					float temp1 = (i + 0.5f) / WIDTH - 0.5f;
					float temp2 = (j + 0.5f) / HEIGHT - 0.5f;
					float distance = 0.5f / tan(camera.arg / 2);
					Ray r;
					r.origin = camera.center;
					r.direction = distance * camera.direction  + temp1 * camera.horizontal + temp2 * camera.up;
					rays.push_back(std::move(r));
				}
			}

		};



		std::vector< Ray > rays;
		std::vector< size_t > leafCounts;
		GENERATERAY(rays);
		for (auto& r : rays) {
			leafCounts.push_back(countLeafIntersect(r));
		}

		double En = 0.0f;
		double En2 = 0.0f;
		for (auto& c : leafCounts) {
			En += c;
			En2 += c * c;
		}
		En /= leafCounts.size();
		En2 /= leafCounts.size();

		return sqrt(En2 - En * En);
	}



	void test() {
		auto directory = Config::GETDIRECTORY();
		auto objname = Config::GETOBJNAME();


		auto triangles = loadObj(directory, objname);
		std::cout << objname << " " << triangles.size() << std::endl;



		auto bvhtype = Config::BVHTYPE();
		std::unique_ptr< CPUBVH > bvh;
		std::cout << bvhtype << std::endl;

		if (bvhtype == "hlbvh") {
			bvh = std::make_unique< HLBVH<CPU> >(triangles);
		}
		else if (bvhtype == "treelet") {
			auto p = std::make_unique< HLBVH<CPU> >(triangles);
			bvh = std::make_unique< TreeletBVH<CPU> >(p->releaseBVH());
		}
		else if (bvhtype == "treeletGPU") {
			goto GPUBVH;
		}

		{
			auto& node = bvh->getBVH();
			auto sah = SAH(node);
			std::cout << "SAH: " << sah << std::endl;

			/*auto epo = EPO(node, triangles);
			std::cout << "EPO: " << epo << std::endl;*/


			OpenCLBasic::init();
			cl::Buffer bvhB = OpenCLBasic::newBuffer<BVHNode>(node.size(), const_cast<BVHNode*>(node.data()));
			cl::Buffer triB = OpenCLBasic::newBuffer<Triangle>(triangles.size(), const_cast<Triangle*>(triangles.data()));

			auto epogpu = EPO_GPU(bvhB, triB);
			std::cout << "EPO_GPU: " << epogpu << std::endl;


			auto camerajson = Config::GETCAMERA();
			if (!camerajson.empty()) {
				auto camera = Auxiliary::parseCamera(camerajson);
				auto lcv = LCV(node, camera);
				std::cout << "LCV: " << lcv << std::endl;
			}
			return;
		}


		// GPU Part
	GPUBVH:
		{
			OpenCLBasic::init();

			auto p = std::make_unique< HLBVH<CPU> >(triangles);
			auto node = p->releaseBVH();

			cl::Buffer bvhB = OpenCLBasic::newBuffer<BVHNode>(node.size(), const_cast<BVHNode*>(node.data()));
			cl::Buffer triB = OpenCLBasic::newBuffer<Triangle>(triangles.size(), const_cast<Triangle*>(triangles.data()));

			auto gpubvh = std::make_unique< TreeletBVH<GPU> >(bvhB, triB);

			OpenCLBasic::readBuffer(gpubvh->getBuffer().first, node.data());
			auto sah = SAH(node);
			std::cout << "SAH: " << sah << std::endl;

			auto tri = OpenCLBasic::readBuffer<Triangle>(triB);
			//auto epo = EPO(node, tri);
			//std::cout << "EPO" << epo << std::endl;
			auto epogpu = EPO_GPU(gpubvh->getBuffer().first, gpubvh->getBuffer().second);
			std::cout << "EPO_GPU: " << epogpu << std::endl;

			auto camerajson = Config::GETCAMERA();
			if (!camerajson.empty()) {
				auto camera = Auxiliary::parseCamera(camerajson);
				auto lcv = LCV(node, camera);
				std::cout << "LCV: " << lcv << std::endl;
			}
		}
	}


	void testmodel(const std::string & objname, json camerajson) {
		auto directory = Config::GETDIRECTORY();


		auto triangles = loadObj(directory, objname);
		std::cout << objname << " " << triangles.size() << std::endl;


		auto bvhtype = Config::BVHTYPE();
		std::unique_ptr< CPUBVH > bvh;
		std::cout << bvhtype << std::endl;

		if (bvhtype == "hlbvh") {
			bvh = std::make_unique< HLBVH<CPU> >(triangles);
		}
		else if (bvhtype == "treelet") {
			auto p = std::make_unique< HLBVH<CPU> >(triangles);
			bvh = std::make_unique< TreeletBVH<CPU> >(p->releaseBVH());
		}
		else if (bvhtype == "treeletGPU") {
			goto GPUBVH;
		}

		{
			auto& node = bvh->getBVH();
			auto sah = SAH(node);
			std::cout << "SAH: " << sah << std::endl;

			/*auto epo = EPO(node, triangles);
			std::cout << "EPO: " << epo << std::endl;*/


			OpenCLBasic::init();
			cl::Buffer bvhB = OpenCLBasic::newBuffer<BVHNode>(node.size(), const_cast<BVHNode*>(node.data()));
			cl::Buffer triB = OpenCLBasic::newBuffer<Triangle>(triangles.size(), const_cast<Triangle*>(triangles.data()));

			auto epogpu = EPO_GPU(bvhB, triB);
			std::cout << "EPO_GPU: " << epogpu << std::endl;

			if (!camerajson.empty()) {
				auto camera = Auxiliary::parseCamera(camerajson);
				auto lcv = LCV(node, camera);
				std::cout << "LCV: " << lcv << std::endl;
			}
			return;
		}


		// GPU Part
	GPUBVH:
		{
			OpenCLBasic::init();

			auto p = std::make_unique< HLBVH<CPU> >(triangles);
			auto node = p->releaseBVH();

			cl::Buffer bvhB = OpenCLBasic::newBuffer<BVHNode>(node.size(), const_cast<BVHNode*>(node.data()));
			cl::Buffer triB = OpenCLBasic::newBuffer<Triangle>(triangles.size(), const_cast<Triangle*>(triangles.data()));

			auto gpubvh = std::make_unique< TreeletBVH<GPU> >(bvhB, triB);

			OpenCLBasic::readBuffer(gpubvh->getBuffer().first, node.data());
			auto sah = SAH(node);
			std::cout << "SAH: " << sah << std::endl;

			auto tri = OpenCLBasic::readBuffer<Triangle>(triB);
			//auto epo = EPO(node, tri);
			//std::cout << "EPO" << epo << std::endl;
			auto epogpu = EPO_GPU(gpubvh->getBuffer().first, gpubvh->getBuffer().second);
			std::cout << "EPO_GPU: " << epogpu << std::endl;

			if (!camerajson.empty()) {
				auto camera = Auxiliary::parseCamera(camerajson);
				auto lcv = LCV(node, camera);
				std::cout << "LCV: " << lcv << std::endl;
			}
		}
	}


	void testall() {
		OpenCLBasic::init();

		auto dir = Config::GETDIRECTORY();
		auto objs = Config::GETOBJS();

		json cmr;
		
		std::vector<std::string> models;
		for (int i = 0; i < objs.size(); ++i) {
			models.push_back(objs[i].get<std::string>());
		}

		std::vector<std::string> models1 = {
			"bmw.obj",
			"spaceship_new.obj",
			"coffee_maker.obj",
			"warriors.obj",
			"dragon.obj", //4
			"sponza.obj",
			//no miguel
			"sibenik.obj",
			"budda.obj" }; //7

		std::vector<std::string> models2 = {
			"conference.obj",
			"dragon-simple.obj",
			"powerplant.obj",
			"san-miguel.obj"
		};

		for (const auto& model : models) {
			testmodel(model,cmr);
			std::cout << std::endl;
		}

	}
}

