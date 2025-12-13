from django.urls import path
from . import views
from .views import IngestReading, LatestReading

urlpatterns = [
    path("", views.home, name="home"),

    path("dashboard/", views.dashboard, name="dashboard"),

    path("api/ingest/", views.IngestReading.as_view(), name="ingest"),
    path("api/latest/<str:device_id>/", views.LatestReading.as_view(), name="latest"),
]