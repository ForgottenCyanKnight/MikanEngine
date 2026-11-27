// SceneSerializer_2D.cpp - 2D 组件的场景序列化（自 SceneSerializer.cpp 拆分，函数体一字未改）
// 与 SceneSerializer_3D.cpp 同属 SceneSerializer 类的成员函数定义（声明见 include/Core/SceneSerializer.h）。
// 拆分目的：改 2D 组件序列化（Sprite2D/Button/Slice9 等）只读本文件。
#include "Core/SceneSerializer.h"
#include "SceneJsonView.h"
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
    json << "        \"respectSafeArea\": " << (component.respectSafeArea ? "true" : "false") << "," << std::endl;
    json << "        \"scaleWithViewport\": " << (component.scaleWithViewport ? "true" : "false") << "," << std::endl;
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
    json << "        \"renderMode\": " << (int)component.renderMode << "," << std::endl;
    json << "        \"anchorMin\": [" << component.anchorMin.x << ", " << component.anchorMin.y << "]," << std::endl;
    json << "        \"anchorMax\": [" << component.anchorMax.x << ", " << component.anchorMax.y << "]," << std::endl;
    json << "        \"pivot\": [" << component.pivot.x << ", " << component.pivot.y << "]" << std::endl;
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
    json << "        \"anchorMin\": [" << component.anchorMin.x << ", " << component.anchorMin.y << "]," << std::endl;
    json << "        \"anchorMax\": [" << component.anchorMax.x << ", " << component.anchorMax.y << "]," << std::endl;
    json << "        \"pivot\": [" << component.pivot.x << ", " << component.pivot.y << "]," << std::endl;
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


void SceneSerializer::DeserializeCanvas2DComponent(Entity entity, SceneJsonView json) {
    const auto canvasJson = json.Find("canvas2d");
    ECS::Canvas2DComponent comp;

    comp.width = canvasJson.Number("width", comp.width);
    comp.height = canvasJson.Number("height", comp.height);
    comp.stretchToViewport = canvasJson.Boolean("stretchToViewport");
    comp.respectSafeArea = canvasJson.Boolean("respectSafeArea");
    comp.scaleWithViewport = canvasJson.Boolean("scaleWithViewport");
    comp.layer = canvasJson.Number("layer", comp.layer);

    auto& coordinator = Coordinator::GetInstance();
    coordinator.AddComponent<ECS::Canvas2DComponent>(entity, std::move(comp));
}

void SceneSerializer::DeserializeSprite2DComponent(Entity entity, SceneJsonView json) {
    const auto spriteJson = json.Find("sprite2d");
    ECS::Sprite2DComponent comp;

    comp.type = (ECS::Sprite2DComponent::Type)spriteJson.Number("type", static_cast<int>(comp.type));
    comp.isUI = spriteJson.Boolean("isUI");
    comp.width = spriteJson.Number("width", comp.width);
    comp.height = spriteJson.Number("height", comp.height);
    std::vector<float> uv0 = spriteJson.FloatArray("uv0");
    if (uv0.size() >= 2) comp.uv0 = glm::vec2(uv0[0], uv0[1]);
    std::vector<float> uv1 = spriteJson.FloatArray("uv1");
    if (uv1.size() >= 2) comp.uv1 = glm::vec2(uv1[0], uv1[1]);
    std::vector<float> color = spriteJson.FloatArray("color");
    if (color.size() >= 4) comp.color = glm::vec4(color[0], color[1], color[2], color[3]);
    std::string texture = spriteJson.String("texture");
    comp.texture = texture;
    comp.layer = spriteJson.Number("layer", comp.layer);
    std::vector<float> anchorMin = spriteJson.FloatArray("anchorMin");
    if (anchorMin.size() >= 2) comp.anchorMin = glm::vec2(anchorMin[0], anchorMin[1]);
    std::vector<float> anchorMax = spriteJson.FloatArray("anchorMax");
    if (anchorMax.size() >= 2) comp.anchorMax = glm::vec2(anchorMax[0], anchorMax[1]);
    std::string label = spriteJson.String("label");
    comp.label = label;
    comp.labelFontSize = spriteJson.Number("labelFontSize", comp.labelFontSize);
    std::vector<float> labelColor = spriteJson.FloatArray("labelColor");
    if (labelColor.size() >= 4) comp.labelColor = glm::vec4(labelColor[0], labelColor[1], labelColor[2], labelColor[3]);

    auto& coordinator = Coordinator::GetInstance();
    coordinator.AddComponent<ECS::Sprite2DComponent>(entity, std::move(comp));
}

void SceneSerializer::DeserializeTextComponent(Entity entity, SceneJsonView json) {
    const auto textJson = json.Find("textComp");
    ECS::TextComponent comp;

    std::string text = textJson.String("text");
    comp.text = text;
    comp.isUI = textJson.Boolean("isUI");
    comp.fontSize = textJson.Number("fontSize", comp.fontSize);
    std::vector<float> color = textJson.FloatArray("color");
    if (color.size() >= 4) comp.color = glm::vec4(color[0], color[1], color[2], color[3]);
    comp.layer = textJson.Number("layer", comp.layer);
    comp.renderMode = (ECS::TextComponent::RenderMode)textJson.Number("renderMode", static_cast<int>(comp.renderMode));
    std::vector<float> anchorMin = textJson.FloatArray("anchorMin");
    if (anchorMin.size() >= 2) comp.anchorMin = glm::vec2(anchorMin[0], anchorMin[1]);
    std::vector<float> anchorMax = textJson.FloatArray("anchorMax");
    if (anchorMax.size() >= 2) comp.anchorMax = glm::vec2(anchorMax[0], anchorMax[1]);
    std::vector<float> pivot = textJson.FloatArray("pivot");
    if (pivot.size() >= 2) comp.pivot = glm::vec2(pivot[0], pivot[1]);

    auto& coordinator = Coordinator::GetInstance();
    coordinator.AddComponent<ECS::TextComponent>(entity, std::move(comp));
}

void SceneSerializer::DeserializeButtonComponent(Entity entity, SceneJsonView json) {
    const auto buttonJson = json.Find("button");
    ECS::ButtonComponent comp;

    comp.isUI = buttonJson.Boolean("isUI");
    comp.width = buttonJson.Number("width", comp.width);
    comp.height = buttonJson.Number("height", comp.height);
    std::vector<float> fillColor = buttonJson.FloatArray("fillColor");
    if (fillColor.size() >= 4) comp.fillColor = glm::vec4(fillColor[0], fillColor[1], fillColor[2], fillColor[3]);
    std::vector<float> hoverColor = buttonJson.FloatArray("hoverColor");
    if (hoverColor.size() >= 4) comp.hoverColor = glm::vec4(hoverColor[0], hoverColor[1], hoverColor[2], hoverColor[3]);
    std::vector<float> textColor = buttonJson.FloatArray("textColor");
    if (textColor.size() >= 4) comp.textColor = glm::vec4(textColor[0], textColor[1], textColor[2], textColor[3]);
    std::string text = buttonJson.String("text");
    comp.text = text;
    comp.fontSize = buttonJson.Number("fontSize", comp.fontSize);
    comp.layer = buttonJson.Number("layer", comp.layer);

    const auto anchorMin = buttonJson.FloatArray("anchorMin");
    const auto anchorMax = buttonJson.FloatArray("anchorMax");
    const auto pivot = buttonJson.FloatArray("pivot");
    if (anchorMin.size() == 2) comp.anchorMin = {anchorMin[0], anchorMin[1]};
    if (anchorMax.size() == 2) comp.anchorMax = {anchorMax[0], anchorMax[1]};
    if (pivot.size() == 2) comp.pivot = {pivot[0], pivot[1]};

    auto& coordinator = Coordinator::GetInstance();
    coordinator.AddComponent<ECS::ButtonComponent>(entity, std::move(comp));
}

void SceneSerializer::DeserializeSlice9Component(Entity entity, SceneJsonView json) {
    const auto sliceJson = json.Find("slice9");
    ECS::Slice9Component comp;

    std::string texture = sliceJson.String("texture");
    comp.texture = texture;
    comp.isUI = sliceJson.Boolean("isUI");
    comp.width = sliceJson.Number("width", comp.width);
    comp.height = sliceJson.Number("height", comp.height);
    std::vector<float> border = sliceJson.FloatArray("border");
    if (border.size() >= 4) comp.border = glm::vec4(border[0], border[1], border[2], border[3]);
    std::vector<float> uv0 = sliceJson.FloatArray("uv0");
    if (uv0.size() >= 2) comp.uv0 = glm::vec2(uv0[0], uv0[1]);
    std::vector<float> uv1 = sliceJson.FloatArray("uv1");
    if (uv1.size() >= 2) comp.uv1 = glm::vec2(uv1[0], uv1[1]);
    std::vector<float> color = sliceJson.FloatArray("color");
    if (color.size() >= 4) comp.color = glm::vec4(color[0], color[1], color[2], color[3]);
    comp.layer = sliceJson.Number("layer", comp.layer);

    auto& coordinator = Coordinator::GetInstance();
    coordinator.AddComponent<ECS::Slice9Component>(entity, std::move(comp));
}

void SceneSerializer::DeserializeTweenComponent(Entity entity, SceneJsonView json) {
    const auto tweenJson = json.Find("tween");
    ECS::TweenComponent comp;

    comp.property = (ECS::TweenComponent::Property)tweenJson.Number("property", static_cast<int>(comp.property));
    comp.easing = (ECS::TweenComponent::Easing)tweenJson.Number("easing", static_cast<int>(comp.easing));
    comp.from = tweenJson.Number("from", comp.from);
    comp.to = tweenJson.Number("to", comp.to);
    comp.duration = tweenJson.Number("duration", comp.duration);
    comp.delay = tweenJson.Number("delay", comp.delay);
    comp.loop = tweenJson.Boolean("loop");
    comp.pingPong = tweenJson.Boolean("pingPong");
    comp.elapsed = 0.0f;
    comp.running = true;
    comp.pingPongReverse = false;

    auto& coordinator = Coordinator::GetInstance();
    coordinator.AddComponent<ECS::TweenComponent>(entity, std::move(comp));
}


} // namespace ECS
