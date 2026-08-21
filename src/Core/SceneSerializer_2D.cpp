// SceneSerializer_2D.cpp - 2D 组件的场景序列化（自 SceneSerializer.cpp 拆分，函数体一字未改）
// 与 SceneSerializer_3D.cpp 同属 SceneSerializer 类的成员函数定义（声明见 include/Core/SceneSerializer.h）。
// 拆分目的：改 2D 组件序列化（Sprite2D/Button/Slice9 等）只读本文件。
#include "Core/SceneSerializer.h"
#include "ECS/Components.h"
#include "ECS/Coordinator.h"
#include "ECS/Types.h"
#include <string>
#include <vector>
#include <sstream>
#include <iostream>
#include <iomanip>

namespace ECS {

std::string SceneSerializer::SerializeCanvas2DComponent(Entity entity) {
    auto& coordinator = Coordinator::GetInstance();
    auto& component = coordinator.GetComponent<ECS::Canvas2DComponent>(entity);
    std::stringstream json;
    json << "      \"canvas2d\": {" << std::endl;
    json << "        \"width\": " << component.width << "," << std::endl;
    json << "        \"height\": " << component.height << "," << std::endl;
    json << "        \"stretchToViewport\": " << (component.stretchToViewport ? "true" : "false") << "," << std::endl;
    json << "        \"layer\": " << component.layer << std::endl;
    json << "      }";
    return json.str();
}

std::string SceneSerializer::SerializeSprite2DComponent(Entity entity) {
    auto& coordinator = Coordinator::GetInstance();
    auto& component = coordinator.GetComponent<ECS::Sprite2DComponent>(entity);
    std::stringstream json;
    json << "      \"sprite2d\": {" << std::endl;
    json << "        \"type\": " << (int)component.type << "," << std::endl;
    json << "        \"isUI\": " << (component.isUI ? "true" : "false") << "," << std::endl;
    json << "        \"width\": " << component.width << "," << std::endl;
    json << "        \"height\": " << component.height << "," << std::endl;
    json << "        \"uv0\": [" << component.uv0.x << ", " << component.uv0.y << "]," << std::endl;
    json << "        \"uv1\": [" << component.uv1.x << ", " << component.uv1.y << "]," << std::endl;
    json << "        \"color\": [" << component.color.x << ", " << component.color.y << ", " << component.color.z << ", " << component.color.w << "]," << std::endl;
    json << "        \"texture\": \"" << EscapeString(component.texture) << "\"," << std::endl;
    json << "        \"layer\": " << component.layer << "," << std::endl;
    json << "        \"anchorMin\": [" << component.anchorMin.x << ", " << component.anchorMin.y << "]," << std::endl;
    json << "        \"anchorMax\": [" << component.anchorMax.x << ", " << component.anchorMax.y << "]," << std::endl;
    json << "        \"label\": \"" << EscapeString(component.label) << "\"," << std::endl;
    json << "        \"labelFontSize\": " << component.labelFontSize << "," << std::endl;
    json << "        \"labelColor\": [" << component.labelColor.x << ", " << component.labelColor.y << ", " << component.labelColor.z << ", " << component.labelColor.w << "]" << std::endl;
    json << "      }";
    return json.str();
}

std::string SceneSerializer::SerializeTextComponent(Entity entity) {
    auto& coordinator = Coordinator::GetInstance();
    auto& component = coordinator.GetComponent<ECS::TextComponent>(entity);
    std::stringstream json;
    //
    json << "      \"textComp\": {" << std::endl;
    json << "        \"text\": \"" << EscapeString(component.text) << "\"," << std::endl;
    json << "        \"isUI\": " << (component.isUI ? "true" : "false") << "," << std::endl;
    json << "        \"fontSize\": " << component.fontSize << "," << std::endl;
    json << "        \"color\": [" << component.color.x << ", " << component.color.y << ", " << component.color.z << ", " << component.color.w << "]," << std::endl;
    json << "        \"layer\": " << component.layer << "," << std::endl;
    json << "        \"renderMode\": " << (int)component.renderMode << std::endl;
    json << "      }";
    return json.str();
}

std::string SceneSerializer::SerializeButtonComponent(Entity entity) {
    auto& coordinator = Coordinator::GetInstance();
    auto& component = coordinator.GetComponent<ECS::ButtonComponent>(entity);
    std::stringstream json;
    json << "      \"button\": {" << std::endl;
    json << "        \"isUI\": " << (component.isUI ? "true" : "false") << "," << std::endl;
    json << "        \"width\": " << component.width << "," << std::endl;
    json << "        \"height\": " << component.height << "," << std::endl;
    json << "        \"fillColor\": [" << component.fillColor.x << ", " << component.fillColor.y << ", " << component.fillColor.z << ", " << component.fillColor.w << "]," << std::endl;
    json << "        \"hoverColor\": [" << component.hoverColor.x << ", " << component.hoverColor.y << ", " << component.hoverColor.z << ", " << component.hoverColor.w << "]," << std::endl;
    json << "        \"textColor\": [" << component.textColor.x << ", " << component.textColor.y << ", " << component.textColor.z << ", " << component.textColor.w << "]," << std::endl;
    json << "        \"text\": \"" << EscapeString(component.text) << "\"," << std::endl;
    json << "        \"fontSize\": " << component.fontSize << "," << std::endl;
    json << "        \"layer\": " << component.layer << std::endl;
    json << "      }";
    return json.str();
}

std::string SceneSerializer::SerializeSlice9Component(Entity entity) {
    auto& coordinator = Coordinator::GetInstance();
    auto& component = coordinator.GetComponent<ECS::Slice9Component>(entity);
    std::stringstream json;
    json << "      \"slice9\": {" << std::endl;
    json << "        \"texture\": \"" << EscapeString(component.texture) << "\"," << std::endl;
    json << "        \"isUI\": " << (component.isUI ? "true" : "false") << "," << std::endl;
    json << "        \"width\": " << component.width << "," << std::endl;
    json << "        \"height\": " << component.height << "," << std::endl;
    json << "        \"border\": [" << component.border.x << ", " << component.border.y << ", " << component.border.z << ", " << component.border.w << "]," << std::endl;
    json << "        \"uv0\": [" << component.uv0.x << ", " << component.uv0.y << "]," << std::endl;
    json << "        \"uv1\": [" << component.uv1.x << ", " << component.uv1.y << "]," << std::endl;
    json << "        \"color\": [" << component.color.x << ", " << component.color.y << ", " << component.color.z << ", " << component.color.w << "]," << std::endl;
    json << "        \"layer\": " << component.layer << std::endl;
    json << "      }";
    return json.str();
}

std::string SceneSerializer::SerializeTweenComponent(Entity entity) {
    auto& coordinator = Coordinator::GetInstance();
    auto& component = coordinator.GetComponent<ECS::TweenComponent>(entity);
    std::stringstream json;
    json << "      \"tween\": {" << std::endl;
    json << "        \"property\": " << (int)component.property << "," << std::endl;
    json << "        \"easing\": " << (int)component.easing << "," << std::endl;
    json << "        \"from\": " << component.from << "," << std::endl;
    json << "        \"to\": " << component.to << "," << std::endl;
    json << "        \"duration\": " << component.duration << "," << std::endl;
    json << "        \"delay\": " << component.delay << "," << std::endl;
    json << "        \"loop\": " << (component.loop ? "true" : "false") << "," << std::endl;
    json << "        \"pingPong\": " << (component.pingPong ? "true" : "false") << std::endl;
    json << "      }";
    return json.str();
}


void SceneSerializer::DeserializeCanvas2DComponent(Entity entity, const std::string& jsonString) {
    std::string canvasJson = ExtractValue(jsonString, "canvas2d");
    ECS::Canvas2DComponent comp;

    std::string wStr = ExtractValue(canvasJson, "width");
    if (!wStr.empty()) { try { comp.width = std::stof(wStr); } catch (...) {} }
    std::string hStr = ExtractValue(canvasJson, "height");
    if (!hStr.empty()) { try { comp.height = std::stof(hStr); } catch (...) {} }
    comp.stretchToViewport = ExtractBoolValue(canvasJson, "stretchToViewport");
    std::string layerStr = ExtractValue(canvasJson, "layer");
    if (!layerStr.empty()) { try { comp.layer = std::stof(layerStr); } catch (...) {} }

    auto& coordinator = Coordinator::GetInstance();
    coordinator.AddComponent<ECS::Canvas2DComponent>(entity, std::move(comp));
}

void SceneSerializer::DeserializeSprite2DComponent(Entity entity, const std::string& jsonString) {
    std::string spriteJson = ExtractValue(jsonString, "sprite2d");
    ECS::Sprite2DComponent comp;

    std::string typeStr = ExtractValue(spriteJson, "type");
    if (!typeStr.empty()) { try { comp.type = (ECS::Sprite2DComponent::Type)std::stoi(typeStr); } catch (...) {} }
    comp.isUI = ExtractBoolValue(spriteJson, "isUI");
    std::string wStr = ExtractValue(spriteJson, "width");
    if (!wStr.empty()) { try { comp.width = std::stof(wStr); } catch (...) {} }
    std::string hStr = ExtractValue(spriteJson, "height");
    if (!hStr.empty()) { try { comp.height = std::stof(hStr); } catch (...) {} }
    std::vector<float> uv0 = ParseFloatArray(ExtractValue(spriteJson, "uv0"));
    if (uv0.size() >= 2) comp.uv0 = glm::vec2(uv0[0], uv0[1]);
    std::vector<float> uv1 = ParseFloatArray(ExtractValue(spriteJson, "uv1"));
    if (uv1.size() >= 2) comp.uv1 = glm::vec2(uv1[0], uv1[1]);
    std::vector<float> color = ParseFloatArray(ExtractValue(spriteJson, "color"));
    if (color.size() >= 4) comp.color = glm::vec4(color[0], color[1], color[2], color[3]);
    std::string texture = ExtractValue(spriteJson, "texture");
    if (!texture.empty() && texture.front() == '"' && texture.back() == '"') texture = texture.substr(1, texture.size() - 2);
    comp.texture = texture;
    std::string layerStr = ExtractValue(spriteJson, "layer");
    if (!layerStr.empty()) { try { comp.layer = std::stoi(layerStr); } catch (...) {} }
    std::vector<float> anchorMin = ParseFloatArray(ExtractValue(spriteJson, "anchorMin"));
    if (anchorMin.size() >= 2) comp.anchorMin = glm::vec2(anchorMin[0], anchorMin[1]);
    std::vector<float> anchorMax = ParseFloatArray(ExtractValue(spriteJson, "anchorMax"));
    if (anchorMax.size() >= 2) comp.anchorMax = glm::vec2(anchorMax[0], anchorMax[1]);
    std::string label = ExtractValue(spriteJson, "label");
    if (!label.empty() && label.front() == '"' && label.back() == '"') label = label.substr(1, label.size() - 2);
    comp.label = label;
    std::string lfsStr = ExtractValue(spriteJson, "labelFontSize");
    if (!lfsStr.empty()) { try { comp.labelFontSize = std::stof(lfsStr); } catch (...) {} }
    std::vector<float> labelColor = ParseFloatArray(ExtractValue(spriteJson, "labelColor"));
    if (labelColor.size() >= 4) comp.labelColor = glm::vec4(labelColor[0], labelColor[1], labelColor[2], labelColor[3]);

    auto& coordinator = Coordinator::GetInstance();
    coordinator.AddComponent<ECS::Sprite2DComponent>(entity, std::move(comp));
}

void SceneSerializer::DeserializeTextComponent(Entity entity, const std::string& jsonString) {
    std::string textJson = ExtractValue(jsonString, "textComp");
    ECS::TextComponent comp;

    std::string text = ExtractValue(textJson, "text");
    if (!text.empty() && text.front() == '"' && text.back() == '"') text = text.substr(1, text.size() - 2);
    comp.text = text;
    comp.isUI = ExtractBoolValue(textJson, "isUI");
    std::string fsStr = ExtractValue(textJson, "fontSize");
    if (!fsStr.empty()) { try { comp.fontSize = std::stof(fsStr); } catch (...) {} }
    std::vector<float> color = ParseFloatArray(ExtractValue(textJson, "color"));
    if (color.size() >= 4) comp.color = glm::vec4(color[0], color[1], color[2], color[3]);
    std::string layerStr = ExtractValue(textJson, "layer");
    if (!layerStr.empty()) { try { comp.layer = std::stoi(layerStr); } catch (...) {} }
    std::string modeStr = ExtractValue(textJson, "renderMode");
    if (!modeStr.empty()) { try { comp.renderMode = (ECS::TextComponent::RenderMode)std::stoi(modeStr); } catch (...) {} }

    auto& coordinator = Coordinator::GetInstance();
    coordinator.AddComponent<ECS::TextComponent>(entity, std::move(comp));
}

void SceneSerializer::DeserializeButtonComponent(Entity entity, const std::string& jsonString) {
    std::string buttonJson = ExtractValue(jsonString, "button");
    ECS::ButtonComponent comp;

    comp.isUI = ExtractBoolValue(buttonJson, "isUI");
    std::string wStr = ExtractValue(buttonJson, "width");
    if (!wStr.empty()) { try { comp.width = std::stof(wStr); } catch (...) {} }
    std::string hStr = ExtractValue(buttonJson, "height");
    if (!hStr.empty()) { try { comp.height = std::stof(hStr); } catch (...) {} }
    std::vector<float> fillColor = ParseFloatArray(ExtractValue(buttonJson, "fillColor"));
    if (fillColor.size() >= 4) comp.fillColor = glm::vec4(fillColor[0], fillColor[1], fillColor[2], fillColor[3]);
    std::vector<float> hoverColor = ParseFloatArray(ExtractValue(buttonJson, "hoverColor"));
    if (hoverColor.size() >= 4) comp.hoverColor = glm::vec4(hoverColor[0], hoverColor[1], hoverColor[2], hoverColor[3]);
    std::vector<float> textColor = ParseFloatArray(ExtractValue(buttonJson, "textColor"));
    if (textColor.size() >= 4) comp.textColor = glm::vec4(textColor[0], textColor[1], textColor[2], textColor[3]);
    std::string text = ExtractValue(buttonJson, "text");
    if (!text.empty() && text.front() == '"' && text.back() == '"') text = text.substr(1, text.size() - 2);
    comp.text = text;
    std::string fsStr = ExtractValue(buttonJson, "fontSize");
    if (!fsStr.empty()) { try { comp.fontSize = std::stof(fsStr); } catch (...) {} }
    std::string layerStr = ExtractValue(buttonJson, "layer");
    if (!layerStr.empty()) { try { comp.layer = std::stoi(layerStr); } catch (...) {} }

    auto& coordinator = Coordinator::GetInstance();
    coordinator.AddComponent<ECS::ButtonComponent>(entity, std::move(comp));
}

void SceneSerializer::DeserializeSlice9Component(Entity entity, const std::string& jsonString) {
    std::string sliceJson = ExtractValue(jsonString, "slice9");
    ECS::Slice9Component comp;

    std::string texture = ExtractValue(sliceJson, "texture");
    if (!texture.empty() && texture.front() == '"' && texture.back() == '"') texture = texture.substr(1, texture.size() - 2);
    comp.texture = texture;
    comp.isUI = ExtractBoolValue(sliceJson, "isUI");
    std::string wStr = ExtractValue(sliceJson, "width");
    if (!wStr.empty()) { try { comp.width = std::stof(wStr); } catch (...) {} }
    std::string hStr = ExtractValue(sliceJson, "height");
    if (!hStr.empty()) { try { comp.height = std::stof(hStr); } catch (...) {} }
    std::vector<float> border = ParseFloatArray(ExtractValue(sliceJson, "border"));
    if (border.size() >= 4) comp.border = glm::vec4(border[0], border[1], border[2], border[3]);
    std::vector<float> uv0 = ParseFloatArray(ExtractValue(sliceJson, "uv0"));
    if (uv0.size() >= 2) comp.uv0 = glm::vec2(uv0[0], uv0[1]);
    std::vector<float> uv1 = ParseFloatArray(ExtractValue(sliceJson, "uv1"));
    if (uv1.size() >= 2) comp.uv1 = glm::vec2(uv1[0], uv1[1]);
    std::vector<float> color = ParseFloatArray(ExtractValue(sliceJson, "color"));
    if (color.size() >= 4) comp.color = glm::vec4(color[0], color[1], color[2], color[3]);
    std::string layerStr = ExtractValue(sliceJson, "layer");
    if (!layerStr.empty()) { try { comp.layer = std::stoi(layerStr); } catch (...) {} }

    auto& coordinator = Coordinator::GetInstance();
    coordinator.AddComponent<ECS::Slice9Component>(entity, std::move(comp));
}

void SceneSerializer::DeserializeTweenComponent(Entity entity, const std::string& jsonString) {
    std::string tweenJson = ExtractValue(jsonString, "tween");
    ECS::TweenComponent comp;

    std::string propStr = ExtractValue(tweenJson, "property");
    if (!propStr.empty()) { try { comp.property = (ECS::TweenComponent::Property)std::stoi(propStr); } catch (...) {} }
    std::string easeStr = ExtractValue(tweenJson, "easing");
    if (!easeStr.empty()) { try { comp.easing = (ECS::TweenComponent::Easing)std::stoi(easeStr); } catch (...) {} }
    std::string fromStr = ExtractValue(tweenJson, "from");
    if (!fromStr.empty()) { try { comp.from = std::stof(fromStr); } catch (...) {} }
    std::string toStr = ExtractValue(tweenJson, "to");
    if (!toStr.empty()) { try { comp.to = std::stof(toStr); } catch (...) {} }
    std::string durStr = ExtractValue(tweenJson, "duration");
    if (!durStr.empty()) { try { comp.duration = std::stof(durStr); } catch (...) {} }
    std::string delayStr = ExtractValue(tweenJson, "delay");
    if (!delayStr.empty()) { try { comp.delay = std::stof(delayStr); } catch (...) {} }
    comp.loop = ExtractBoolValue(tweenJson, "loop");
    comp.pingPong = ExtractBoolValue(tweenJson, "pingPong");
    comp.elapsed = 0.0f;
    comp.running = true;
    comp.pingPongReverse = false;

    auto& coordinator = Coordinator::GetInstance();
    coordinator.AddComponent<ECS::TweenComponent>(entity, std::move(comp));
}


} // namespace ECS
