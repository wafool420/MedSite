# medsite/urls.py
from django.urls import path
from . import views

urlpatterns = [
    path("", views.home, name="home"),
    path("api/ingest/", views.api_ingest),

    path("api/latest/p/<int:patient_id>/", views.api_latest_patient, name="api_latest_patient"),
    path("api/latest/<str:public_code>/", views.api_latest, name="api_latest"),

    path("stats/<int:patient_id>/", views.stats_page, name="stats"),
    path("patients/create/", views.create_patient, name="create_patient"),
    path("patients/<int:patient_id>/", views.patient_detail, name="patient_detail"),
    path("patients/<int:patient_id>/archive/", views.archive_patient, name="archive_patient"),
    path("patients/<int:patient_id>/restore/", views.unarchive_patient, name="unarchive_patient"),
    path("m/<str:public_code>/", views.public_monitor, name="public_monitor"),
    
    
    path("login/", views.login_view, name="login"),
    path("register/", views.register_view, name="register"),
    path("logout/", views.logout_view, name="logout"),
]
