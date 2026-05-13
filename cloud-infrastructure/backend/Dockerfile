# =============================================================================
# KnockKnock - Cloud Backend Dockerfile
# =============================================================================
FROM python:3.11-slim

# Prevent .pyc files and enable unbuffered stdout for real-time logging.
ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

WORKDIR /app

# Install system dependencies (if any needed in the future).
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        curl \
    && rm -rf /var/lib/apt/lists/*

# Install Python dependencies.
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Copy application code.
COPY app/ ./app/

# Run the application.
CMD ["python", "-m", "app.main"]
