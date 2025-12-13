from rest_framework import serializers
from .models import Reading

class ReadingIngestSerializer(serializers.ModelSerializer):
    class Meta:
        model = Reading
        fields = [
            "device_id",
            "ir", "red", "finger",
            "bpm", "spo2", "pi", "rr", "sbp", "dbp", "temp",
        ]
