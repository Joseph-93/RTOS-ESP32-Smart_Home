// Banner notification system
class NotificationManager {
    constructor() {
        this.container = null;
        this.init();
    }

    init() {
        // Create notification container if it doesn't exist
        if (!this.container) {
            this.container = document.createElement('div');
            this.container.id = 'notification-container';
            this.container.style.cssText = `
                position: fixed;
                top: 20px;
                right: 20px;
                z-index: 10000;
                display: flex;
                flex-direction: column;
                gap: 10px;
                max-width: 400px;
            `;
            // Wait for body to be available
            if (document.body) {
                document.body.appendChild(this.container);
            } else {
                document.addEventListener('DOMContentLoaded', () => {
                    document.body.appendChild(this.container);
                });
            }
        }
    }

    show(message, type = 'success', duration = 4000) {
        const banner = document.createElement('div');
        banner.className = `notification-banner notification-${type}`;
        
        const bgColor = type === 'success' ? '#10b981' : '#ef4444';
        const icon = type === 'success' ? '✓' : '✗';
        
        banner.style.cssText = `
            background-color: ${bgColor};
            color: white;
            padding: 12px 16px;
            border-radius: 6px;
            box-shadow: 0 4px 6px rgba(0,0,0,0.1);
            display: flex;
            align-items: center;
            gap: 10px;
            font-size: 14px;
            animation: slideIn 0.3s ease-out;
            cursor: pointer;
        `;
        
        banner.innerHTML = `
            <span style="font-weight: bold; font-size: 16px;">${icon}</span>
            <span style="flex: 1;">${message}</span>
        `;
        
        // Add slide-in animation
        const style = document.createElement('style');
        style.textContent = `
            @keyframes slideIn {
                from {
                    transform: translateX(400px);
                    opacity: 0;
                }
                to {
                    transform: translateX(0);
                    opacity: 1;
                }
            }
            @keyframes slideOut {
                from {
                    transform: translateX(0);
                    opacity: 1;
                }
                to {
                    transform: translateX(400px);
                    opacity: 0;
                }
            }
        `;
        if (!document.querySelector('#notification-animations')) {
            style.id = 'notification-animations';
            document.head.appendChild(style);
        }
        
        // Click to dismiss
        banner.addEventListener('click', () => {
            this.dismiss(banner);
        });
        
        this.container.appendChild(banner);
        
        // Auto dismiss after duration
        if (duration > 0) {
            setTimeout(() => {
                this.dismiss(banner);
            }, duration);
        }
        
        return banner;
    }

    dismiss(banner) {
        banner.style.animation = 'slideOut 0.3s ease-out';
        setTimeout(() => {
            if (banner.parentNode) {
                banner.parentNode.removeChild(banner);
            }
        }, 300);
    }

    success(message, duration = 4000) {
        return this.show(message, 'success', duration);
    }

    error(message, duration = 4000) {
        return this.show(message, 'error', duration);
    }
}

// Global instance - use window to ensure it's accessible everywhere
window.notifications = new NotificationManager();
