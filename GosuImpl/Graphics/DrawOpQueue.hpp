#ifndef GOSUIMPL_GRAPHICS_DRAWOPQUEUE_HPP
#define GOSUIMPL_GRAPHICS_DRAWOPQUEUE_HPP

#include <Gosu/TR1.hpp>
#include <GosuImpl/Graphics/Common.hpp>
#include <GosuImpl/Graphics/TransformStack.hpp>
#include <GosuImpl/Graphics/ClipRectStack.hpp>
#include <GosuImpl/Graphics/DrawOp.hpp>
#include <cassert>
#include <algorithm>
#include <map>
#include <vector>

class Gosu::DrawOpQueue
{
    TransformStack transformStack;
    ClipRectStack clipRectStack;
    
    typedef std::vector<DrawOp> DrawOps;
    DrawOps ops;
    typedef std::vector<std::tr1::function<void()> > GLBlocks;
    GLBlocks glBlocks;
    
public:
    void scheduleDrawOp(DrawOp op)
    {
        if (clipRectStack.clippedWorldAway())
            return;
        
        #ifdef GOSU_IS_IPHONE
        // No triangles, no lines supported
        assert (op.verticesOrBlockIndex == 4);
        #endif
        
        op.renderState.transform = &transformStack.current();
        if (const ClipRect* cr = clipRectStack.maybeEffectiveRect())
            op.renderState.clipRect = *cr;
        ops.push_back(op);
    }
    
    void scheduleGL(std::tr1::function<void()> glBlock, ZPos z)
    {
        // TODO: Document this case: Clipped-away GL blocks are *not* being run.
        if (clipRectStack.clippedWorldAway())
            return;
        
        int complementOfBlockIndex = ~(int)glBlocks.size();
        glBlocks.push_back(glBlock);
        
        DrawOp op;
        op.verticesOrBlockIndex = complementOfBlockIndex;
        op.renderState.transform = &transformStack.current();
        if (const ClipRect* cr = clipRectStack.maybeEffectiveRect())
            op.renderState.clipRect = *cr;
        op.z = z;
        ops.push_back(op);
    }
    
    void beginClipping(int x, int y, int width, int height, int screenHeight)
    {
        // Apply current transformation.
        
        double left = x, right = x + width;
        double top = y, bottom = y + height;
        
        applyTransform(transformStack.current(), left, top);
        applyTransform(transformStack.current(), right, bottom);
        
        int physX = std::min(left, right);
        int physY = std::min(top, bottom);
        int physWidth = std::abs(int(left - right));
        int physHeight = std::abs(int(top - bottom));
        
        // Adjust for OpenGL having the wrong idea of where y=0 is.
        // TODO: This should really happen *right before* setting up
        // the glScissor.
        physY = screenHeight - physY - physHeight;
        
        clipRectStack.beginClipping(physX, physY, physWidth, physHeight);
    }
    
    void endClipping()
    {
        clipRectStack.endClipping();
    }
    
    void beginStencil(int x, int y, int width, int height, int screenHeight)
    {
        // Apply current transformation.
        
        double left = x, right = x + width;
        double top = y, bottom = y + height;
        
        applyTransform(transformStack.current(), left, top);
        applyTransform(transformStack.current(), right, bottom);
        
        int physX = std::min(left, right);
        int physY = std::min(top, bottom);
        int physWidth = std::abs(int(left - right));
        int physHeight = std::abs(int(top - bottom));
        
        // Adjust for OpenGL having the wrong idea of where y=0 is.
        // TODO: This should really happen *right before* setting up
        // the glScissor.
        physY = screenHeight - physY - physHeight;
        
        clipRectStack.beginClipping(physX, physY, physWidth, physHeight);
        
        // Much code taken from the wikibooks OpenGl Stencil Buffer page
        // Enable stencil buffer
		glEnable(GL_STENCIL_TEST);
		
		// Disable color and depth buffers
		glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
		glDepthMask(GL_FALSE);
		
		glStencilFunc(GL_NEVER, 1, all_planes);
		glStencilOp(GL_REPLACE, GL_KEEP, GL_KEEP); // Draw 1s on test fail (always)
		
		// Draw stencil pattern
		glStencilMask(all_planes);
		glClear(GL_STENCIL_BUFFER_BIT);
		
		// ===== Draw mask
		mask.call
		
		// Re-enable color and depth buffers
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glDepthMask(GL_TRUE);
		glStencilMask(no_planes);
		
		// Draw where stencil's value is 0
		glStencilFunc(GL_EQUAL, 0, all_planes);
		// (nothing to draw)
		
		// Draw only where stencil's value is 1
		glStencilFunc(GL_EQUAL, 1, all_planes);
		
		// Call the block to be drawn after this
    }
    
    void endStencil()
    {
        // Turn stencil buffer off
        
		glDisable(GL_STENCIL_TEST);
        clipRectStack.endClipping();
    }
    
    void setBaseTransform(const Transform& baseTransform)
    {
        transformStack.setBaseTransform(baseTransform);
    }
    
    void pushTransform(const Transform& transform)
    {
        transformStack.push(transform);
    }
    
    void popTransform()
    {
        transformStack.pop();
    }
    
    void performDrawOpsAndCode()
    {
        // Apply Z-Ordering.
        std::stable_sort(ops.begin(), ops.end());
        
        RenderStateManager manager;
        #ifdef GOSU_IS_IPHONE
        if (ops.empty())
            return;
        
        DrawOps::const_iterator current = ops.begin(), last = ops.end() - 1;
        for (; current != last; ++current)
        {
            manager.setRenderState(current->renderState);
            current->perform(&*(current + 1));
        }
        manager.setRenderState(last->renderState);
        last->perform(0);
        #else
        for (DrawOps::const_iterator current = ops.begin(), last = ops.end();
            current != last; ++current)
        {
            manager.setRenderState(current->renderState);
            if (current->verticesOrBlockIndex >= 0)
                current->perform(0);
            else
            {
                // GL code
                int blockIndex = ~current->verticesOrBlockIndex;
                assert (blockIndex >= 0);
                assert (blockIndex < glBlocks.size());
                glBlocks[blockIndex]();
                manager.enforceAfterUntrustedGL();
            }
        }
        #endif
    }
    
    void compileTo(VertexArrays& vas)
    {
        if (!glBlocks.empty())
            throw std::logic_error("Custom code cannot be recorded into a macro");
        
        std::stable_sort(ops.begin(), ops.end());
        for (DrawOps::const_iterator op = ops.begin(), end = ops.end(); op != end; ++op)
            op->compileTo(vas);
    }
    
    // This retains the current stack of transforms and clippings.
    void clearQueue()
    {
        glBlocks.clear();
        ops.clear();
    }
    
    // This clears the queue and starts with new stacks. This must not be called
    // when endClipping/popTransform calls might still be pending.
    void reset()
    {
        transformStack.reset();
        clipRectStack.clear();
        clearQueue();
    }
};

#endif
