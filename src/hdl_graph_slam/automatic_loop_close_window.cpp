#include <hdl_graph_slam/automatic_loop_close_window.hpp>

#include <g2o/core/robust_kernel_factory.h>
#include <g2o/types/slam3d/edge_se3.h>
#include <g2o/types/slam3d/vertex_se3.h>

#include <pclomp/ndt_omp.h>
#include <hdl_graph_slam/information_matrix_calculator.hpp>

namespace hdl_graph_slam {

AutomaticLoopCloseWindow::AutomaticLoopCloseWindow(InteractiveGraphView& graph, const std::string& data_directory)
: show_window(false),
graph(graph),
running(false),
loop_detection_source(0),
scan_matching_method(0),
scan_matching_resolution(2.0f),
fitness_score_thresh(0.3f),
fitness_score_max_range(2.0f),
distance_thresh(10.0f),
accum_distance_thresh(15.0f),
robust_kernel(1),
robust_kernel_delta(0.1)
{}

AutomaticLoopCloseWindow::~AutomaticLoopCloseWindow() {
  if(running){
    running = false;
    loop_detection_thread.join();
  }
}

void AutomaticLoopCloseWindow::draw_ui() {
  if(!show_window) {
    running = false;
    return;
  }

  ImGui::Begin("automatic loop close", &show_window, ImGuiWindowFlags_AlwaysAutoResize);

  ImGui::Text("Scan matching");
  const char* methods[] = { "NDT" };
  ImGui::Combo("method", &scan_matching_method, methods, IM_ARRAYSIZE(methods));
  ImGui::DragFloat("Resolution", &scan_matching_resolution, 0.01f, 0.01f, 20.0f);
  ImGui::DragFloat("Fitness score thresh", &fitness_score_thresh, 0.01f, 0.01f, 10.0f);

  ImGui::Text("Loop detection");
  ImGui::DragFloat("Distance thresh", &distance_thresh, 0.01f, 0.01f, 100.0f);
  ImGui::DragFloat("Accum distance thresh", &accum_distance_thresh, 0.01f, 0.01f, 100.0f);

  ImGui::Text("Robust kernel");
  const char* kernels[] = {"NONE", "Huber"};
  ImGui::Combo("Kernel type", &robust_kernel, kernels, IM_ARRAYSIZE(kernels));
  ImGui::DragFloat("Kernel delta", &robust_kernel_delta, 0.01f, 0.01f, 10.0f);

  if(ImGui::Button("Start")) {
    if(!running) {
      running = true;
      loop_detection_thread = std::thread([&]() { loop_detection(); });
    }
  }

  ImGui::SameLine();
  if(ImGui::Button("Stop")) {
    if(running) {
      running = false;
      loop_detection_thread.join();
    }
  }

  if(running) {
    loop_detection_source ++;
    if(loop_detection_source == graph.keyframes_view.size()) {
      loop_detection_source = 0;
    }
  }

  ImGui::End();
}

void AutomaticLoopCloseWindow::loop_detection() {
  pcl::Registration<pcl::PointXYZI, pcl::PointXYZI>::Ptr registration;
  auto ndt = boost::make_shared<pclomp::NormalDistributionsTransform<pcl::PointXYZI, pcl::PointXYZI>>();
  ndt->setResolution(scan_matching_resolution);
  registration = ndt;

  while (running) {
    KeyFrameView::Ptr source = graph.keyframes_view[loop_detection_source];
    Eigen::Isometry3d source_pose = source->lock()->node->estimate();
    auto candidates = find_loop_candidates(source);

    {
      std::lock_guard<std::mutex> lock(loop_detection_mutex);
      loop_source = source;
      loop_candidates = candidates;
    }

    bool edge_inserted = false;
    ndt->setInputTarget(source->lock()->cloud);
    for(int i=0; i<candidates.size(); i++) {
      ndt->setInputSource(candidates[i]->lock()->cloud);

      pcl::PointCloud<pcl::PointXYZI>::Ptr aligned(new pcl::PointCloud<pcl::PointXYZI>());
      Eigen::Isometry3d relative = source_pose.inverse() * candidates[i]->lock()->node->estimate();
      registration->align(*aligned, relative.matrix().cast<float>());

      relative.matrix() = registration->getFinalTransformation().cast<double>();
      double fitness_score = InformationMatrixCalculator::calc_fitness_score(source->lock()->cloud, candidates[i]->lock()->cloud, relative, fitness_score_max_range);

      if (fitness_score < fitness_score_thresh) {
        const char* kernels[] = {"NONE", "Huber"};
  
        edge_inserted = true;
        auto edge = graph.add_edge(source->lock(), candidates[i]->lock(), relative, kernels[robust_kernel], robust_kernel_delta);

      }
    }

    if(edge_inserted) {
      std::lock_guard<std::mutex> lock(graph.optimization_mutex);
      graph.optimize();
    }

    loop_detection_source++;
    if(loop_detection_source == graph.keyframes_view.size()) {
      loop_detection_source = 0;
    }
  }
}

std::vector<KeyFrameView::Ptr> AutomaticLoopCloseWindow::find_loop_candidates(const KeyFrameView::Ptr& keyframe) {
  std::unordered_map<long, float> accum_distances;
  accum_distances[keyframe->lock()->id()] = 0.0f;

  std::deque<KeyFrameView::Ptr> search_queue = {keyframe};

  while(!search_queue.empty()) {
    auto target = search_queue.front()->lock();
    float target_accum_distance = accum_distances[target->id()];
    Eigen::Vector3d target_pos = target->estimate().translation();
    search_queue.pop_front();

    for(auto& edge_: target->node->edges()) {
      g2o::EdgeSE3* edge = dynamic_cast<g2o::EdgeSE3*>(edge_);
      if(edge == nullptr) {
        continue;
      }

      g2o::VertexSE3* v1 = dynamic_cast<g2o::VertexSE3*>(edge->vertices()[0]);
      g2o::VertexSE3* v2 = dynamic_cast<g2o::VertexSE3*>(edge->vertices()[1]);

      g2o::VertexSE3* next = (v1->id() == target->node->id()) ? v2 : v1;
      if(graph.keyframes.find(next->id()) == graph.keyframes.end()) {
        continue;
      }

      float delta = (next->estimate().translation() - target_pos).norm();
      float accum_distance = target_accum_distance + delta;

      auto found = accum_distances.find(next->id());
      if(found == accum_distances.end() || found->second > accum_distance) {
        accum_distances[next->id()] = accum_distance;
        search_queue.push_back(graph.keyframes_view_map[graph.keyframes[next->id()]]);
      }
    }
  }

  Eigen::Vector3d keyframe_pos = keyframe->lock()->node->estimate().translation();
  std::vector<KeyFrameView::Ptr> loop_candidates;
  for(const auto& candidate: graph.keyframes_view) {
    auto found = accum_distances.find(candidate->lock()->id());
    if(found == accum_distances.end()) {
      continue;
    }

    double accum_dist = found->second;
    double dist = (candidate->lock()->node->estimate().translation() - keyframe_pos).norm();

    if(accum_dist > accum_distance_thresh && dist < distance_thresh) {
      loop_candidates.push_back(candidate);
    }
  }

  return loop_candidates;
}

void AutomaticLoopCloseWindow::draw_gl(glk::GLSLShader& shader) {
  if(!running) {
    return;
  }

  std::lock_guard<std::mutex> lock(loop_detection_mutex);
  if (loop_source == nullptr) {
    return;
  }
  
  shader.set_uniform("color_mode", 1);
  shader.set_uniform("point_size", 100.0f);


  loop_source->draw(shader, Eigen::Vector4f(0.0f, 0.0f, 1.0f, 1.0f), loop_source->lock()->node->estimate().matrix().cast<float>());

  for (const auto& candidate : loop_candidates) {
    candidate->draw(shader, Eigen::Vector4f(0.0f, 1.0f, 0.0f, 1.0f), candidate->lock()->node->estimate().matrix().cast<float>());
  }
}

void AutomaticLoopCloseWindow::show() {
  show_window = true;
}
}