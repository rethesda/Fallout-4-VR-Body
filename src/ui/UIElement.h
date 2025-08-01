#pragma once

#include "UIModAdapter.h"
#include "../common/Quaternion.h"

namespace vrui
{
    struct UISize
    {
        float width;
        float height;

        UISize(const float width, const float height) :
            width(width), height(height) {}
    };

    class UIElement
    {
    public:
        UIElement()
        {
            _transform.translate = RE::NiPoint3(0, 0, 0);
            _transform.rotate = common::getIdentityMatrix();
            _transform.scale = 1;
        }

        virtual ~UIElement() = default;

        /**
         * Set the position of the UI element relative to the parent.
         * @param x horizontal: positive-right, negative-left
         * @param y depth: positive-forward, negative-backward
         * @param z vertical: positive-up, negative-down
         */
        void setPosition(const float x, const float y, const float z) { _transform.translate = RE::NiPoint3(x, y, z); }
        void updatePosition(const float x, const float y, const float z) { _transform.translate += RE::NiPoint3(x, y, z); }
        const RE::NiPoint3& getPosition() const { return _transform.translate; }

        float getScale() const { return _transform.scale; }
        void setScale(const float scale) { _transform.scale = scale; }

        bool isVisible() const { return _visible; }
        void setVisibility(const bool visible) { _visible = visible; }

        const UISize& getSize() const { return _size; }
        void setSize(const UISize size) { _size = size; }
        void setSize(const float width, const float height) { _size = { width, height }; }

        UIElement* getParent() const { return _parent; }
        void setParent(UIElement* parent) { _parent = parent; }

        virtual std::string toString() const;

        // Internal:
        virtual void onLayoutUpdate(UIFrameUpdateContext*) {}

        // Internal: Handle UI interaction code on each frame of the game.
        virtual void onFrameUpdate(UIFrameUpdateContext* context) = 0;

        // NOTE: those can be called a lot, shouldn't be an issue for our usage but maybe worth checking one day
        // Internal: Calculate if the element should be visible with respect to all parents.
        bool calcVisibility() const { return _visible && (_parent ? _parent->calcVisibility() : true); }
        // Internal: Calculate the scale adjustment of the element with respect to all parents.
        float calcScale() const { return _transform.scale * (_parent ? _parent->calcScale() : 1); }
        // Internal: Calculate the size with relation to scale with respect to all parents.
        UISize calcSize() const
        {
            const auto scale = calcScale();
            return { _size.width * scale, _size.height * scale };
        }

    protected:
        virtual RE::NiTransform calculateTransform() const;
        virtual void onPressEventFired(UIElement*, UIFrameUpdateContext*) {}
        void onPressEventFiredPropagate(UIElement* element, UIFrameUpdateContext* context);
        virtual void onStateChanged(UIElement* element);

        // Attach the UI element to the given game node.
        virtual void attachToNode(RE::NiNode* attachNode);
        virtual void detachFromAttachedNode(bool releaseSafe);

        UIElement* _parent = nullptr;
        RE::NiTransform _transform;
        bool _visible = true;

        // the width (x) and height (y) of the widget
        UISize _size = UISize(0, 0);

        // Game node the main node is attached to
        RE::NiPointer<RE::NiNode> _attachNode = nullptr;

        // Used to allow hiding attachToNode, detachFromAttachedNode from public API
        friend class UIManager;
    };
}
