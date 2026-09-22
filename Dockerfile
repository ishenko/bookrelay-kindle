FROM python:3.11-slim
WORKDIR /app
COPY relay/requirements.txt /app/relay/requirements.txt
RUN pip install --no-cache-dir -r /app/relay/requirements.txt
COPY relay /app/relay
COPY kpm /app/kpm
ENV PYTHONPATH=/app/relay
ENV BOOKRELAY_DB=/data/relay.sqlite3
VOLUME ["/data"]
EXPOSE 8000
CMD ["uvicorn", "bookrelay.main:app", "--host", "0.0.0.0", "--port", "8000"]
